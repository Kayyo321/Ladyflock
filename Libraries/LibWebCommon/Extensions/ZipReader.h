/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/Span.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWebCommon/Export.h>

namespace Web::Extensions {

// Hardened ZIP/XPI reader for untrusted extension archives.
class WEBCOMMON_API ZipReader {
public:
    static constexpr size_t max_entries = 8192;
    static constexpr u64 max_entry_uncompressed_bytes = 32 * MiB;
    static constexpr u64 max_total_uncompressed_bytes = 128 * MiB;
    static constexpr u64 max_compression_ratio = 200;

    struct Entry {
        String name;
        u16 compression_method { 0 };
        u32 crc32 { 0 };
        u32 compressed_size { 0 };
        u32 uncompressed_size { 0 };
        u32 local_header_offset { 0 };
        bool is_directory { false };
    };

    static ErrorOr<ZipReader> parse(ReadonlyBytes archive_bytes);
    static ErrorOr<String> validate_entry_name(StringView raw_name);

    Vector<Entry> const& entries() const { return m_entries; }
    Optional<Entry const&> find_entry(StringView name) const;
    ErrorOr<ByteBuffer> extract_entry(Entry const&) const;
    ErrorOr<ByteBuffer> extract_entry_by_name(StringView name) const;

private:
    ZipReader(ReadonlyBytes bytes, Vector<Entry> entries, HashMap<String, size_t> index_by_name)
        : m_bytes(bytes)
        , m_entries(move(entries))
        , m_index_by_name(move(index_by_name))
    {
    }

    ReadonlyBytes m_bytes;
    Vector<Entry> m_entries;
    HashMap<String, size_t> m_index_by_name;
};

}
