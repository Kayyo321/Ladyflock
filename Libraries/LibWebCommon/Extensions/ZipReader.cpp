/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/Endian.h>
#include <AK/ScopeGuard.h>
#include <LibWebCommon/Extensions/ZipReader.h>
#include <zlib.h>

namespace Web::Extensions {

static u16 read_le16(ReadonlyBytes bytes, size_t offset)
{
    return static_cast<u16>(bytes[offset]) | (static_cast<u16>(bytes[offset + 1]) << 8);
}

static u32 read_le32(ReadonlyBytes bytes, size_t offset)
{
    return static_cast<u32>(bytes[offset])
        | (static_cast<u32>(bytes[offset + 1]) << 8)
        | (static_cast<u32>(bytes[offset + 2]) << 16)
        | (static_cast<u32>(bytes[offset + 3]) << 24);
}

ErrorOr<String> ZipReader::validate_entry_name(StringView raw_name)
{
    if (raw_name.is_empty())
        return Error::from_string_literal("ZIP entry has empty name");
    if (raw_name.contains('\0') || raw_name.contains('\\'))
        return Error::from_string_literal("ZIP entry contains illegal character");
    if (raw_name.starts_with('/'))
        return Error::from_string_literal("ZIP entry has absolute path");

    bool is_directory = raw_name.ends_with('/');
    auto trimmed = is_directory ? raw_name.substring_view(0, raw_name.length() - 1) : raw_name;
    if (trimmed.is_empty())
        return Error::from_string_literal("ZIP entry resolves to empty path");

    for (auto segment : trimmed.split_view('/', SplitBehavior::KeepEmpty)) {
        if (segment.is_empty() || segment == "."sv || segment == ".."sv)
            return Error::from_string_literal("ZIP entry contains unsafe path segment");
    }

    return String::from_utf8(raw_name);
}

ErrorOr<ZipReader> ZipReader::parse(ReadonlyBytes archive_bytes)
{
    if (archive_bytes.size() < 22)
        return Error::from_string_literal("Archive too small to be a valid ZIP file");

    // Search backwards for End of Central Directory signature (0x06054b50) within max comment range.
    size_t min_eocd_offset = archive_bytes.size() > 65557 ? archive_bytes.size() - 65557 : 0;
    Optional<size_t> eocd_offset;
    for (size_t pos = archive_bytes.size() - 22;; --pos) {
        if (read_le32(archive_bytes, pos) == 0x06054b50) {
            u16 comment_len = read_le16(archive_bytes, pos + 20);
            if (pos + 22 + comment_len == archive_bytes.size()) {
                eocd_offset = pos;
                break;
            }
        }
        if (pos == min_eocd_offset)
            break;
    }

    if (!eocd_offset.has_value())
        return Error::from_string_literal("ZIP End of Central Directory record not found");

    u16 disk_number = read_le16(archive_bytes, *eocd_offset + 4);
    u16 cd_disk = read_le16(archive_bytes, *eocd_offset + 6);
    u16 entries_on_disk = read_le16(archive_bytes, *eocd_offset + 8);
    u16 total_entries = read_le16(archive_bytes, *eocd_offset + 10);
    u32 cd_size = read_le32(archive_bytes, *eocd_offset + 12);
    u32 cd_offset = read_le32(archive_bytes, *eocd_offset + 16);

    if (disk_number != 0 || cd_disk != 0 || entries_on_disk != total_entries)
        return Error::from_string_literal("Multi-disk ZIP archives are not supported");
    if (total_entries > max_entries)
        return Error::from_string_literal("ZIP archive exceeds maximum entry count");

    Checked<size_t> cd_end = cd_offset;
    cd_end += cd_size;
    if (cd_end.has_overflow() || cd_end.value() > *eocd_offset)
        return Error::from_string_literal("ZIP Central Directory extends past EOCD");

    Vector<Entry> entries;
    entries.ensure_capacity(total_entries);
    HashMap<String, size_t> index_by_name;
    u64 total_uncompressed = 0;

    size_t cursor = cd_offset;
    for (u16 i = 0; i < total_entries; ++i) {
        if (cursor + 46 > cd_end.value())
            return Error::from_string_literal("Truncated ZIP Central Directory header");
        if (read_le32(archive_bytes, cursor) != 0x02014b50)
            return Error::from_string_literal("Invalid ZIP Central Directory signature");

        u16 flags = read_le16(archive_bytes, cursor + 8);
        u16 compression_method = read_le16(archive_bytes, cursor + 10);
        u32 crc32_val = read_le32(archive_bytes, cursor + 16);
        u32 compressed_size = read_le32(archive_bytes, cursor + 20);
        u32 uncompressed_size = read_le32(archive_bytes, cursor + 24);
        u16 name_len = read_le16(archive_bytes, cursor + 28);
        u16 extra_len = read_le16(archive_bytes, cursor + 30);
        u16 comment_len = read_le16(archive_bytes, cursor + 32);
        u32 external_attrs = read_le32(archive_bytes, cursor + 38);
        u32 local_header_offset = read_le32(archive_bytes, cursor + 42);

        if (flags & 0x0001)
            return Error::from_string_literal("Encrypted ZIP entries are not supported");

        Checked<size_t> header_end = cursor;
        header_end += 46;
        header_end += name_len;
        header_end += extra_len;
        header_end += comment_len;
        if (header_end.has_overflow() || header_end.value() > cd_end.value())
            return Error::from_string_literal("ZIP Central Directory entry extends past Central Directory");

        auto raw_name = StringView { archive_bytes.slice(cursor + 46, name_len) };
        auto name = TRY(validate_entry_name(raw_name));

        if (index_by_name.contains(name))
            return Error::from_string_literal("Duplicate ZIP entry name");

        // Reject symlinks (POSIX S_IFLNK = 0120000 in upper 16 bits of external attributes).
        u16 unix_mode = static_cast<u16>((external_attrs >> 16) & 0xF000);
        if (unix_mode == 0xA000)
            return Error::from_string_literal("Symlinks in ZIP archives are forbidden");

        if (compression_method != 0 && compression_method != 8)
            return Error::from_string_literal("Unsupported ZIP compression method (only store and deflate supported)");

        if (uncompressed_size > max_entry_uncompressed_bytes)
            return Error::from_string_literal("ZIP entry exceeds maximum allowed uncompressed size");

        if (compressed_size > 0 && uncompressed_size / compressed_size > max_compression_ratio && uncompressed_size > 64 * KiB)
            return Error::from_string_literal("ZIP entry exceeds maximum allowed compression ratio");

        total_uncompressed += uncompressed_size;
        if (total_uncompressed > max_total_uncompressed_bytes)
            return Error::from_string_literal("ZIP archive exceeds maximum total uncompressed size");

        if (static_cast<size_t>(local_header_offset) + 30 > cd_offset)
            return Error::from_string_literal("ZIP local header offset out of bounds");

        bool is_directory = name.bytes_as_string_view().ends_with('/');
        index_by_name.set(name, entries.size());
        entries.unchecked_append(Entry {
            .name = move(name),
            .compression_method = compression_method,
            .crc32 = crc32_val,
            .compressed_size = compressed_size,
            .uncompressed_size = uncompressed_size,
            .local_header_offset = local_header_offset,
            .is_directory = is_directory,
        });

        cursor = header_end.value();
    }

    if (cursor != cd_end.value())
        return Error::from_string_literal("ZIP Central Directory trailing bytes mismatch");

    return ZipReader(archive_bytes, move(entries), move(index_by_name));
}

Optional<ZipReader::Entry const&> ZipReader::find_entry(StringView name) const
{
    auto idx = m_index_by_name.get(name);
    if (!idx.has_value())
        return {};
    return m_entries[*idx];
}

ErrorOr<ByteBuffer> ZipReader::extract_entry_by_name(StringView name) const
{
    auto entry = find_entry(name);
    if (!entry.has_value())
        return Error::from_string_literal("ZIP entry not found");
    return extract_entry(*entry);
}

ErrorOr<ByteBuffer> ZipReader::extract_entry(Entry const& entry) const
{
    if (entry.is_directory)
        return ByteBuffer {};

    size_t lfh = entry.local_header_offset;
    if (lfh + 30 > m_bytes.size())
        return Error::from_string_literal("Truncated ZIP local file header");
    if (read_le32(m_bytes, lfh) != 0x04034b50)
        return Error::from_string_literal("Invalid ZIP local file header signature");

    u16 lfh_name_len = read_le16(m_bytes, lfh + 26);
    u16 lfh_extra_len = read_le16(m_bytes, lfh + 28);

    Checked<size_t> data_offset = lfh;
    data_offset += 30;
    data_offset += lfh_name_len;
    data_offset += lfh_extra_len;
    if (data_offset.has_overflow())
        return Error::from_string_literal("ZIP local header offset overflow");

    Checked<size_t> data_end = data_offset.value();
    data_end += entry.compressed_size;
    if (data_end.has_overflow() || data_end.value() > m_bytes.size())
        return Error::from_string_literal("ZIP compressed payload extends past end of archive");

    auto compressed = m_bytes.slice(data_offset.value(), entry.compressed_size);
    auto output = TRY(ByteBuffer::create_uninitialized(entry.uncompressed_size));

    if (entry.compression_method == 0) {
        if (entry.compressed_size != entry.uncompressed_size)
            return Error::from_string_literal("Stored ZIP entry compressed/uncompressed size mismatch");
        if (!compressed.is_empty())
            compressed.copy_to(output.bytes());
    } else if (entry.compression_method == 8) {
        if (entry.uncompressed_size == 0) {
            // Still verify CRC below.
        } else {
            z_stream zs {};
            if (inflateInit2(&zs, -MAX_WBITS) != Z_OK)
                return Error::from_string_literal("Failed to initialize zlib inflate for ZIP entry");
            ScopeGuard end_inflate = [&] { inflateEnd(&zs); };

            zs.next_in = const_cast<Bytef*>(compressed.data());
            zs.avail_in = static_cast<uInt>(compressed.size());
            zs.next_out = output.data();
            zs.avail_out = static_cast<uInt>(output.size());

            int ret = inflate(&zs, Z_FINISH);
            if (ret != Z_STREAM_END || zs.total_out != entry.uncompressed_size || zs.avail_in != 0)
                return Error::from_string_literal("Failed to inflate ZIP entry cleanly");
        }
    } else {
        return Error::from_string_literal("Unsupported ZIP compression method");
    }

    uLong computed_crc = crc32(0L, Z_NULL, 0);
    if (!output.is_empty())
        computed_crc = crc32(computed_crc, output.data(), static_cast<uInt>(output.size()));
    if (static_cast<u32>(computed_crc) != entry.crc32)
        return Error::from_string_literal("ZIP entry CRC-32 verification failed");

    return output;
}

}
