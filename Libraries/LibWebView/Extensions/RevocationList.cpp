/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebView/Extensions/RevocationList.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

namespace WebView::Extensions {

ErrorOr<void> RevocationList::load_bundled(ByteString const& resource_root)
{
    auto envelope_path = LexicalPath::join(resource_root, "extension-revocations.json"sv).string();
    auto key_path = LexicalPath::join(resource_root, "extension-revocations.pem"sv).string();
    if (!FileSystem::exists(envelope_path) && !FileSystem::exists(key_path))
        return {};

    auto read_bounded = [](ByteString const& path, size_t max_size) -> ErrorOr<ByteBuffer> {
        auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read | Core::File::OpenMode::Nonblocking | Core::File::OpenMode::NoFollow));
        auto metadata = TRY(file->stat());
        if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0 || static_cast<u64>(metadata.st_size) > max_size)
            return Error::from_string_literal("Invalid bundled extension revocation file");
        auto bytes = TRY(ByteBuffer::create_uninitialized(static_cast<size_t>(metadata.st_size)));
        TRY(file->read_until_filled(bytes.bytes()));
        return bytes;
    };
    auto key = TRY(read_bounded(key_path, 16 * KiB));
    auto envelope = TRY(read_bounded(envelope_path, 2 * MiB));
    return load_signed_envelope(StringView { envelope.bytes() }, StringView { key.bytes() });
}

static ErrorOr<void> verify_signature(StringView payload_bytes, StringView signature_base64, StringView public_key_pem)
{
    auto signature = TRY(decode_base64(signature_base64));

    auto* bio = BIO_new_mem_buf(public_key_pem.bytes().data(), static_cast<int>(public_key_pem.bytes().size()));
    if (!bio)
        return Error::from_string_literal("Failed allocating BIO for revocation public key");
    ScopeGuard free_bio = [&] { BIO_free(bio); };

    EVP_PKEY* pkey = PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
    if (!pkey)
        return Error::from_string_literal("Failed parsing revocation public key PEM");
    ScopeGuard free_pkey = [&] { EVP_PKEY_free(pkey); };

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx)
        return Error::from_string_literal("Failed allocating EVP_MD_CTX");
    ScopeGuard free_ctx = [&] { EVP_MD_CTX_free(ctx); };

    // Ed25519 / Ed448 take nullptr digest; RSA/ECDSA use SHA-256.
    int key_type = EVP_PKEY_base_id(pkey);
    EVP_MD const* md = (key_type == EVP_PKEY_ED25519 || key_type == EVP_PKEY_ED448) ? nullptr : EVP_sha256();

    if (EVP_DigestVerifyInit(ctx, nullptr, md, nullptr, pkey) != 1)
        return Error::from_string_literal("EVP_DigestVerifyInit failed");

    int ok = EVP_DigestVerify(
        ctx,
        signature.data(),
        signature.size(),
        reinterpret_cast<unsigned char const*>(payload_bytes.bytes().data()),
        payload_bytes.bytes().size());
    if (ok != 1)
        return Error::from_string_literal("Revocation list signature verification failed");

    return {};
}

ErrorOr<void> RevocationList::load_signed_json(StringView payload_json, StringView signature_base64, StringView public_key_pem)
{
    TRY(verify_signature(payload_json, signature_base64, public_key_pem));

    auto parsed = TRY(JsonValue::from_string(payload_json));
    if (!parsed.is_object())
        return Error::from_string_literal("Revocation payload must be a JSON object");

    auto const& obj = parsed.as_object();
    auto entries_val = obj.get_array("entries"sv);
    if (!entries_val.has_value())
        return Error::from_string_literal("Revocation payload missing 'entries' array");

    Vector<RevocationEntry> new_entries;
    for (auto const& item : entries_val->values()) {
        if (!item.is_object())
            return Error::from_string_literal("Revocation entry must be an object");
        auto const& entry_obj = item.as_object();
        RevocationEntry entry;
        entry.addon_id = entry_obj.get_string("addon_id"sv).value_or({});
        if (auto v = entry_obj.get_string("version"sv); v.has_value() && !v->is_empty())
            entry.version = *v;
        if (auto h = entry_obj.get_string("sha256"sv); h.has_value() && !h->is_empty())
            entry.sha256_hex = *h;
        entry.reason = entry_obj.get_string("reason"sv).value_or("Revoked by blocklist"_string);
        if (entry.addon_id.is_empty() && !entry.sha256_hex.has_value())
            return Error::from_string_literal("Revocation entry must specify addon_id or sha256");
        new_entries.append(move(entry));
    }

    m_entries = move(new_entries);
    return {};
}

ErrorOr<void> RevocationList::load_signed_envelope(StringView envelope_json, StringView public_key_pem)
{
    auto parsed = TRY(JsonValue::from_string(envelope_json));
    if (!parsed.is_object())
        return Error::from_string_literal("Revocation envelope must be a JSON object");

    auto const& obj = parsed.as_object();
    auto payload = obj.get_string("payload"sv);
    auto signature = obj.get_string("signature"sv);
    if (!payload.has_value() || !signature.has_value())
        return Error::from_string_literal("Revocation envelope must contain 'payload' and 'signature' strings");

    return load_signed_json(*payload, *signature, public_key_pem);
}

bool RevocationList::is_revoked(StringView addon_id, Optional<StringView> version, Optional<StringView> sha256_hex) const
{
    for (auto const& entry : m_entries) {
        if (sha256_hex.has_value() && entry.sha256_hex.has_value() && *entry.sha256_hex == *sha256_hex)
            return true;
        if (!entry.addon_id.is_empty() && entry.addon_id == addon_id) {
            if (!entry.version.has_value())
                return true;
            if (version.has_value() && *entry.version == *version)
                return true;
        }
    }
    return false;
}

Vector<String> RevocationList::revoked_addon_ids() const
{
    Vector<String> ids;
    for (auto const& entry : m_entries) {
        if (!entry.addon_id.is_empty() && !entry.version.has_value())
            ids.append(entry.addon_id);
    }
    return ids;
}

Vector<String> RevocationList::revoked_sha256_hexes() const
{
    Vector<String> hashes;
    for (auto const& entry : m_entries) {
        if (entry.sha256_hex.has_value())
            hashes.append(*entry.sha256_hex);
    }
    return hashes;
}

}
