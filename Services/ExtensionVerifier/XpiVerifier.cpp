/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Base64.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <ExtensionVerifier/XpiVerifier.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>
#include <LibWebCommon/Extensions/SupportMatrix.h>
#include <LibWebCommon/Extensions/ZipReader.h>
#include <openssl/bio.h>
#include <openssl/cms.h>
#include <openssl/core_names.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

namespace ExtensionVerifier {

static Web::Extensions::XpiVerificationResult make_error(String message)
{
    Web::Extensions::XpiVerificationResult res;
    res.ok = false;
    res.error = move(message);
    return res;
}

static ErrorOr<ByteBuffer> compute_digest(ReadonlyBytes data, EVP_MD const* md)
{
    auto* ctx = EVP_MD_CTX_new();
    if (!ctx)
        return Error::from_string_literal("Out of memory allocating EVP_MD_CTX");
    ScopeGuard free_ctx = [&] { EVP_MD_CTX_free(ctx); };

    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1)
        return Error::from_string_literal("EVP_DigestInit_ex failed");
    if (!data.is_empty() && EVP_DigestUpdate(ctx, data.data(), data.size()) != 1)
        return Error::from_string_literal("EVP_DigestUpdate failed");

    unsigned int len = 0;
    auto out = TRY(ByteBuffer::create_uninitialized(EVP_MAX_MD_SIZE));
    if (EVP_DigestFinal_ex(ctx, out.data(), &len) != 1)
        return Error::from_string_literal("EVP_DigestFinal_ex failed");
    out.resize(len);
    return out;
}

static ErrorOr<String> sha256_hex_string(ReadonlyBytes data)
{
    auto digest = TRY(compute_digest(data, EVP_sha256()));
    StringBuilder builder;
    for (u8 byte : digest.bytes())
        builder.appendff("{:02x}", byte);
    return builder.to_string();
}

static Vector<ByteString> unfold_manifest_lines(StringView raw)
{
    Vector<ByteString> lines;
    ByteString normalized = ByteString(raw).replace("\r\n"sv, "\n"sv, ReplaceMode::All);
    for (auto line : normalized.split_view('\n', SplitBehavior::KeepEmpty)) {
        if (line.starts_with(' ') && !lines.is_empty()) {
            lines.last() = ByteString::formatted("{}{}", lines.last(), line.substring_view(1));
        } else {
            lines.append(line);
        }
    }
    return lines;
}

struct ManifestEntryDigests {
    Optional<ByteBuffer> sha256;
    Optional<ByteBuffer> sha1;
};

static ErrorOr<HashMap<String, ManifestEntryDigests>> parse_manifest_mf(StringView text)
{
    HashMap<String, ManifestEntryDigests> entries;
    auto lines = unfold_manifest_lines(text);

    bool in_main_header = true;
    bool saw_manifest_version = false;
    Optional<String> current_name;
    ManifestEntryDigests current_digests;

    auto flush_entry = [&]() -> ErrorOr<void> {
        if (!current_name.has_value())
            return {};
        if (!current_digests.sha256.has_value() && !current_digests.sha1.has_value())
            return Error::from_string_literal("manifest.mf entry missing digest");
        if (entries.contains(*current_name))
            return Error::from_string_literal("Duplicate entry in manifest.mf");
        entries.set(*current_name, move(current_digests));
        current_name.clear();
        current_digests = {};
        return {};
    };

    for (auto const& line : lines) {
        if (line.is_empty()) {
            TRY(flush_entry());
            in_main_header = false;
            continue;
        }
        auto colon = line.find(':');
        if (!colon.has_value())
            return Error::from_string_literal("Malformed line in manifest.mf");
        auto key = line.substring_view(0, *colon).trim_whitespace();
        auto val = line.substring_view(*colon + 1).trim_whitespace();

        if (in_main_header && key.equals_ignoring_ascii_case("Manifest-Version"sv)) {
            saw_manifest_version = true;
            continue;
        }
        if (key.equals_ignoring_ascii_case("Name"sv)) {
            if (current_name.has_value())
                return Error::from_string_literal("Nested Name header in manifest.mf section");
            current_name = TRY(Web::Extensions::ZipReader::validate_entry_name(val));
            in_main_header = false;
        } else if (key.equals_ignoring_ascii_case("SHA256-Digest"sv)) {
            auto decoded = TRY(decode_base64(val));
            if (decoded.size() != 32)
                return Error::from_string_literal("Invalid SHA256-Digest length in manifest.mf");
            current_digests.sha256 = move(decoded);
        } else if (key.equals_ignoring_ascii_case("SHA1-Digest"sv)) {
            auto decoded = TRY(decode_base64(val));
            if (decoded.size() != 20)
                return Error::from_string_literal("Invalid SHA1-Digest length in manifest.mf");
            current_digests.sha1 = move(decoded);
        }
    }
    TRY(flush_entry());

    if (!saw_manifest_version)
        return Error::from_string_literal("manifest.mf missing Manifest-Version header");
    return entries;
}

static ErrorOr<void> verify_mozilla_sf(StringView sf_text, ReadonlyBytes manifest_mf_bytes, bool require_sha256)
{
    auto lines = unfold_manifest_lines(sf_text);
    Optional<ByteBuffer> sha256_manifest;
    Optional<ByteBuffer> sha1_manifest;

    for (auto const& line : lines) {
        if (line.is_empty())
            break;
        auto colon = line.find(':');
        if (!colon.has_value())
            continue;
        auto key = line.substring_view(0, *colon).trim_whitespace();
        auto val = line.substring_view(*colon + 1).trim_whitespace();
        if (key.equals_ignoring_ascii_case("SHA256-Digest-Manifest"sv)) {
            sha256_manifest = TRY(decode_base64(val));
        } else if (key.equals_ignoring_ascii_case("SHA1-Digest-Manifest"sv)) {
            sha1_manifest = TRY(decode_base64(val));
        }
    }

    if (!sha256_manifest.has_value() && !sha1_manifest.has_value())
        return Error::from_string_literal("mozilla.sf missing SHA256-Digest-Manifest / SHA1-Digest-Manifest");
    if (require_sha256 && !sha256_manifest.has_value())
        return Error::from_string_literal("XPI CMS signature manifest is missing a SHA-256 digest");

    if (sha256_manifest.has_value()) {
        auto actual = TRY(compute_digest(manifest_mf_bytes, EVP_sha256()));
        if (actual.bytes() != sha256_manifest->bytes())
            return Error::from_string_literal("manifest.mf SHA-256 mismatch with mozilla.sf");
    }
    if (sha1_manifest.has_value()) {
        auto actual = TRY(compute_digest(manifest_mf_bytes, EVP_sha1()));
        if (actual.bytes() != sha1_manifest->bytes())
            return Error::from_string_literal("manifest.mf SHA-1 mismatch with mozilla.sf");
    }
    return {};
}

static ErrorOr<String> extract_common_name(X509* cert)
{
    if (!cert)
        return Error::from_string_literal("Missing signer certificate");
    auto* subject = X509_get_subject_name(cert);
    if (!subject)
        return Error::from_string_literal("Signer certificate missing subject");

    int idx = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
    if (idx < 0)
        return Error::from_string_literal("Signer certificate missing Common Name");
    auto* entry = X509_NAME_get_entry(subject, idx);
    auto* asn1 = X509_NAME_ENTRY_get_data(entry);
    unsigned char* utf8 = nullptr;
    int len = ASN1_STRING_to_UTF8(&utf8, asn1);
    if (len < 0 || !utf8)
        return Error::from_string_literal("Failed converting signer CN to UTF-8");
    ScopeGuard free_utf8 = [&] { OPENSSL_free(utf8); };
    return TRY(String::from_utf8(StringView { reinterpret_cast<char const*>(utf8), static_cast<size_t>(len) }));
}

static ErrorOr<X509_STORE*> make_trust_store(Vector<String> const& trusted_root_pems)
{
    if (trusted_root_pems.is_empty())
        return Error::from_string_literal("No trusted root CA certificates configured for XPI verification");

    X509_STORE* store = X509_STORE_new();
    if (!store)
        return Error::from_string_literal("Failed allocating X509_STORE");
    ArmedScopeGuard free_store = [&] { X509_STORE_free(store); };

    auto* parameters = X509_STORE_get0_param(store);
    // Firefox deliberately permits expired certificates for its isolated add-on
    // signing root. Public XPI validity is otherwise bounded by revocation data.
    X509_VERIFY_PARAM_set_flags(parameters, X509_V_FLAG_NO_CHECK_TIME);
    X509_VERIFY_PARAM_set_auth_level(parameters, 2);
    X509_STORE_set_purpose(store, X509_PURPOSE_ANY);

    size_t loaded_roots = 0;
    for (auto const& pem : trusted_root_pems) {
        auto* bio = BIO_new_mem_buf(pem.bytes().data(), static_cast<int>(pem.bytes().size()));
        if (!bio)
            continue;
        ScopeGuard free_bio = [&] { BIO_free(bio); };
        while (X509* cert = PEM_read_bio_X509_AUX(bio, nullptr, nullptr, nullptr)) {
            ScopeGuard free_cert = [&] { X509_free(cert); };
            if (X509_STORE_add_cert(store, cert) == 1)
                ++loaded_roots;
        }
        ERR_clear_error();
    }

    if (loaded_roots == 0)
        return Error::from_string_literal("Failed to parse any trusted root CA certificates");
    free_store.disarm();
    return store;
}

static ErrorOr<void> verify_signer_usage(X509* certificate)
{
    if (!certificate || X509_check_ca(certificate) != 0
        || X509_get_ext_by_NID(certificate, NID_key_usage, -1) < 0
        || X509_get_ext_by_NID(certificate, NID_ext_key_usage, -1) < 0
        || !(X509_get_key_usage(certificate) & KU_DIGITAL_SIGNATURE)
        || !(X509_get_extended_key_usage(certificate) & XKU_CODE_SIGN))
        return Error::from_string_literal("XPI certificate is not a code-signing leaf");
    return {};
}

struct CmsVerification {
    String signer;
    bool uses_sha256 { false };
};

static ErrorOr<CmsVerification> verify_mozilla_rsa(ReadonlyBytes rsa_der_bytes, ReadonlyBytes sf_bytes, X509_STORE* store)
{
    auto* rsa_bio = BIO_new_mem_buf(rsa_der_bytes.data(), static_cast<int>(rsa_der_bytes.size()));
    if (!rsa_bio)
        return Error::from_string_literal("Failed allocating BIO for mozilla.rsa");
    ScopeGuard free_rsa_bio = [&] { BIO_free(rsa_bio); };

    auto* cms = d2i_CMS_bio(rsa_bio, nullptr);
    if (!cms || BIO_ctrl_pending(rsa_bio) != 0) {
        CMS_ContentInfo_free(cms);
        return Error::from_string_literal("Could not decode mozilla.rsa as CMS SignedData");
    }
    ScopeGuard free_cms = [&] { CMS_ContentInfo_free(cms); };
    auto* sf_bio = BIO_new_mem_buf(sf_bytes.data(), static_cast<int>(sf_bytes.size()));
    if (!sf_bio)
        return Error::from_string_literal("Failed allocating BIO for mozilla.sf");
    ScopeGuard free_sf_bio = [&] { BIO_free(sf_bio); };

    if (CMS_verify(cms, nullptr, store, sf_bio, nullptr, CMS_DETACHED | CMS_BINARY) != 1)
        return Error::from_string_literal("XPI CMS signature verification failed");

    auto* signers = CMS_get0_signers(cms);
    ScopeGuard free_signers = [&] { sk_X509_free(signers); };
    auto* signer_infos = CMS_get0_SignerInfos(cms);
    if (sk_X509_num(signers) != 1 || sk_CMS_SignerInfo_num(signer_infos) != 1)
        return Error::from_string_literal("Unsupported XPI CMS signer set");
    auto* signer = sk_X509_value(signers, 0);
    TRY(verify_signer_usage(signer));
    X509_ALGOR* digest = nullptr;
    CMS_SignerInfo_get0_algs(sk_CMS_SignerInfo_value(signer_infos, 0), nullptr, nullptr, &digest, nullptr);
    auto algorithm = digest ? OBJ_obj2nid(digest->algorithm) : NID_undef;
    if (algorithm != NID_sha256 && algorithm != NID_sha1)
        return Error::from_string_literal("Unsupported XPI CMS digest algorithm");
    return CmsVerification { TRY(extract_common_name(signer)), algorithm == NID_sha256 };
}

// Accept the definite-length CBOR subset used by Mozilla's COSE_Sign archives
// (RFC 9052). The signed bytes retain the original protected headers verbatim.
class CoseReader {
public:
    explicit CoseReader(ReadonlyBytes bytes)
        : m_bytes(bytes)
    {
    }

    ErrorOr<u64> read_head(u8 type)
    {
        if (m_bytes.is_empty() || (m_bytes[0] >> 5) != type)
            return Error::from_string_literal("Invalid XPI COSE structure");
        u8 argument = m_bytes[0] & 31;
        m_bytes = m_bytes.slice(1);
        if (argument < 24)
            return argument;
        if (argument > 27)
            return Error::from_string_literal("Unsupported XPI COSE length");
        auto width = 1u << (argument - 24);
        if (width > m_bytes.size())
            return Error::from_string_literal("Truncated XPI COSE argument");
        u64 value = 0;
        for (auto byte : m_bytes.slice(0, width))
            value = (value << 8) | byte;
        m_bytes = m_bytes.slice(width);
        return value;
    }

    ErrorOr<void> expect(u8 type, u64 value)
    {
        if (TRY(read_head(type)) != value)
            return Error::from_string_literal("Unexpected XPI COSE structure");
        return {};
    }

    ErrorOr<ReadonlyBytes> bytes()
    {
        auto size = TRY(read_head(2));
        if (size > m_bytes.size())
            return Error::from_string_literal("Truncated XPI COSE byte string");
        auto result = m_bytes.slice(0, static_cast<size_t>(size));
        m_bytes = m_bytes.slice(static_cast<size_t>(size));
        return result;
    }

    bool at_end() const { return m_bytes.is_empty(); }

private:
    ReadonlyBytes m_bytes;
};

static ErrorOr<void> append_cose_field(ByteBuffer& result, u8 type, ReadonlyBytes contents)
{
    auto size = contents.size();
    auto head = static_cast<u8>(type << 5);
    if (size < 24) {
        TRY(result.try_append(head | static_cast<u8>(size)));
    } else if (size <= 0xff) {
        TRY(result.try_append(head | 24));
        TRY(result.try_append(static_cast<u8>(size)));
    } else if (size <= 0xffff) {
        TRY(result.try_append(head | 25));
        TRY(result.try_append(static_cast<u8>(size >> 8)));
        TRY(result.try_append(static_cast<u8>(size)));
    } else {
        TRY(result.try_append(head | 26));
        for (auto shift : { 24, 16, 8, 0 })
            TRY(result.try_append(static_cast<u8>(size >> shift)));
    }
    return result.try_append(contents);
}

static ErrorOr<X509*> parse_cose_certificate(ReadonlyBytes bytes)
{
    auto* position = bytes.data();
    auto* certificate = d2i_X509(nullptr, &position, static_cast<long>(bytes.size()));
    if (!certificate || position != bytes.data() + bytes.size()) {
        X509_free(certificate);
        return Error::from_string_literal("Invalid XPI COSE certificate");
    }
    return certificate;
}

static ErrorOr<ByteBuffer> encode_ecdsa_signature(ReadonlyBytes raw_signature, size_t component_size)
{
    if (raw_signature.size() != 2 * component_size)
        return Error::from_string_literal("Invalid XPI COSE ECDSA signature size");
    auto* signature = ECDSA_SIG_new();
    if (!signature)
        return Error::from_string_literal("Failed allocating XPI COSE ECDSA signature");
    ScopeGuard free_signature = [&] { ECDSA_SIG_free(signature); };
    auto* r = BN_bin2bn(raw_signature.data(), static_cast<int>(component_size), nullptr);
    auto* s = BN_bin2bn(raw_signature.data() + component_size, static_cast<int>(component_size), nullptr);
    if (!r || !s || ECDSA_SIG_set0(signature, r, s) != 1) {
        BN_free(r);
        BN_free(s);
        return Error::from_string_literal("Invalid XPI COSE ECDSA signature");
    }
    auto length = i2d_ECDSA_SIG(signature, nullptr);
    if (length <= 0)
        return Error::from_string_literal("Could not serialize XPI COSE ECDSA signature");
    auto result = TRY(ByteBuffer::create_uninitialized(static_cast<size_t>(length)));
    auto* position = result.data();
    if (i2d_ECDSA_SIG(signature, &position) != length)
        return Error::from_string_literal("Could not serialize XPI COSE ECDSA signature");
    return result;
}

static ErrorOr<void> verify_cose_signature(ReadonlyBytes manifest, ReadonlyBytes signature, X509_STORE* store, String const& expected_signer)
{
    CoseReader envelope(signature);
    TRY(envelope.expect(6, 98)); // COSE_Sign tag
    TRY(envelope.expect(4, 4));
    auto body_protected = TRY(envelope.bytes());
    TRY(envelope.expect(5, 0));  // Unprotected headers
    TRY(envelope.expect(7, 22)); // Detached payload

    CoseReader body(body_protected);
    TRY(body.expect(5, 1));
    TRY(body.expect(0, 4)); // Mozilla's certificate-chain header
    auto certificate_count = TRY(body.read_head(4));
    if (certificate_count > 8)
        return Error::from_string_literal("XPI COSE certificate chain exceeds the supported size");
    auto* intermediates = sk_X509_new_null();
    if (!intermediates)
        return Error::from_string_literal("Failed allocating XPI COSE certificate chain");
    ScopeGuard free_intermediates = [&] { sk_X509_pop_free(intermediates, X509_free); };
    for (size_t index = 0; index < certificate_count; ++index) {
        auto* intermediate = TRY(parse_cose_certificate(TRY(body.bytes())));
        if (sk_X509_push(intermediates, intermediate) == 0) {
            X509_free(intermediate);
            return Error::from_string_literal("Failed adding XPI COSE certificate");
        }
    }
    if (!body.at_end())
        return Error::from_string_literal("Unexpected XPI COSE body header field");

    auto signature_count = TRY(envelope.read_head(4));
    if (signature_count == 0 || signature_count > 4)
        return Error::from_string_literal("Unsupported XPI COSE signer set");
    for (size_t index = 0; index < signature_count; ++index) {
        TRY(envelope.expect(4, 3));
        auto signer_protected = TRY(envelope.bytes());
        TRY(envelope.expect(5, 0));
        auto raw_signature = TRY(envelope.bytes());
        CoseReader signer_header(signer_protected);
        TRY(signer_header.expect(5, 2));
        TRY(signer_header.expect(0, 1)); // Algorithm
        auto algorithm = TRY(signer_header.read_head(1));
        TRY(signer_header.expect(0, 4)); // Signer certificate
        auto* signer = TRY(parse_cose_certificate(TRY(signer_header.bytes())));
        ScopeGuard free_signer = [&] { X509_free(signer); };
        if (!signer_header.at_end() || TRY(extract_common_name(signer)) != expected_signer)
            return Error::from_string_literal("XPI COSE signer identity does not match the archive");
        TRY(verify_signer_usage(signer));

        auto* trust = X509_STORE_CTX_new();
        if (!trust)
            return Error::from_string_literal("Failed allocating XPI COSE certificate verifier");
        ScopeGuard free_trust = [&] { X509_STORE_CTX_free(trust); };
        if (X509_STORE_CTX_init(trust, store, signer, intermediates) != 1 || X509_verify_cert(trust) != 1)
            return Error::from_string_literal("XPI COSE certificate verification failed");

        auto* key = X509_get0_pubkey(signer);
        if (!key || EVP_PKEY_is_a(key, "EC") != 1)
            return Error::from_string_literal("XPI COSE signer has no ECDSA key");
        EVP_MD const* digest = nullptr;
        StringView curve;
        size_t component_size = 0;
        switch (algorithm) {
        case 6: // ES256 (-7)
            digest = EVP_sha256();
            curve = "prime256v1"sv;
            component_size = 32;
            break;
        case 34: // ES384 (-35)
            digest = EVP_sha384();
            curve = "secp384r1"sv;
            component_size = 48;
            break;
        case 35: // ES512 (-36)
            digest = EVP_sha512();
            curve = "secp521r1"sv;
            component_size = 66;
            break;
        default:
            return Error::from_string_literal("Unsupported XPI COSE signature algorithm");
        }
        char group[64];
        size_t group_length = 0;
        if (EVP_PKEY_get_utf8_string_param(key, OSSL_PKEY_PARAM_GROUP_NAME, group, sizeof(group), &group_length) != 1
            || StringView { group, group_length } != curve)
            return Error::from_string_literal("XPI COSE signature algorithm does not match the certificate key");

        auto der_signature = TRY(encode_ecdsa_signature(raw_signature, component_size));
        ByteBuffer signed_payload;
        TRY(signed_payload.try_append(0x85)); // Sig_structure array
        TRY(append_cose_field(signed_payload, 3, "Signature"sv.bytes()));
        TRY(append_cose_field(signed_payload, 2, body_protected));
        TRY(append_cose_field(signed_payload, 2, signer_protected));
        // Mozilla's XPI COSE profile serializes absent external AAD as null.
        // See third_party/rust/cose/src/util.rs in the Firefox source tree.
        TRY(signed_payload.try_append(0xf6));
        TRY(append_cose_field(signed_payload, 2, manifest));
        auto* verification = EVP_MD_CTX_new();
        if (!verification)
            return Error::from_string_literal("Failed allocating XPI COSE verifier");
        ScopeGuard free_verification = [&] { EVP_MD_CTX_free(verification); };
        if (EVP_DigestVerifyInit(verification, nullptr, digest, nullptr, key) != 1
            || EVP_DigestVerify(verification, der_signature.data(), der_signature.size(), signed_payload.data(), signed_payload.size()) != 1)
            return Error::from_string_literal("XPI COSE signature verification failed");
    }
    if (!envelope.at_end())
        return Error::from_string_literal("Unexpected bytes following XPI COSE signature");
    return {};
}

Web::Extensions::XpiVerificationResult verify_xpi_bytes(
    ReadonlyBytes xpi_bytes,
    Optional<String> const& expected_addon_id,
    Vector<String> const& trusted_root_pems,
    Vector<String> const& revoked_addon_ids,
    Vector<String> const& revoked_sha256_hexes,
    Vector<String>& file_names,
    Vector<ByteBuffer>& file_contents)
{
    auto sha256_or_error = sha256_hex_string(xpi_bytes);
    if (sha256_or_error.is_error())
        return make_error(MUST(String::formatted("SHA-256 computation failed: {}", sha256_or_error.error())));
    auto sha256_hex = sha256_or_error.release_value();

    if (revoked_sha256_hexes.contains_slow(sha256_hex))
        return make_error("Extension package hash is on the revocation list"_string);

    auto zip_or_error = Web::Extensions::ZipReader::parse(xpi_bytes);
    if (zip_or_error.is_error())
        return make_error(MUST(String::formatted("Invalid XPI archive: {}", zip_or_error.error())));
    auto zip = zip_or_error.release_value();

    auto rsa_or_error = zip.extract_entry_by_name("META-INF/mozilla.rsa"sv);
    if (rsa_or_error.is_error())
        return make_error("XPI missing META-INF/mozilla.rsa signature"_string);
    auto sf_or_error = zip.extract_entry_by_name("META-INF/mozilla.sf"sv);
    if (sf_or_error.is_error())
        return make_error("XPI missing META-INF/mozilla.sf signature manifest"_string);
    auto mf_or_error = zip.extract_entry_by_name("META-INF/manifest.mf"sv);
    if (mf_or_error.is_error())
        return make_error("XPI missing META-INF/manifest.mf file manifest"_string);

    auto trust_or_error = make_trust_store(trusted_root_pems);
    if (trust_or_error.is_error())
        return make_error(MUST(String::formatted("Signature verification failed: {}", trust_or_error.error())));
    auto* trust = trust_or_error.release_value();
    ScopeGuard free_trust = [&] { X509_STORE_free(trust); };
    auto cms_or_error = verify_mozilla_rsa(rsa_or_error.value().bytes(), sf_or_error.value().bytes(), trust);
    if (cms_or_error.is_error())
        return make_error(MUST(String::formatted("Signature verification failed: {}", cms_or_error.error())));
    auto cms = cms_or_error.release_value();

    Optional<HashMap<String, ManifestEntryDigests>> cose_entries;
    bool has_cose = zip.find_entry("META-INF/cose.sig"sv).has_value() || zip.find_entry("META-INF/cose.manifest"sv).has_value();
    if (has_cose) {
        auto cose_manifest = zip.extract_entry_by_name("META-INF/cose.manifest"sv);
        auto cose_signature = zip.extract_entry_by_name("META-INF/cose.sig"sv);
        if (cose_manifest.is_error() || cose_signature.is_error())
            return make_error("XPI COSE signature is incomplete"_string);
        if (auto err = verify_cose_signature(cose_manifest.value().bytes(), cose_signature.value().bytes(), trust, cms.signer); err.is_error())
            return make_error(MUST(String::formatted("COSE signature verification failed: {}", err.error())));
        auto entries = parse_manifest_mf(StringView { cose_manifest.value().bytes() });
        if (entries.is_error())
            return make_error(MUST(String::formatted("Invalid XPI COSE file manifest: {}", entries.error())));
        cose_entries = entries.release_value();
    } else if (!cms.uses_sha256) {
        return make_error("XPI is missing a strong signature"_string);
    }

    auto sf_text_or_error = String::from_utf8(StringView { sf_or_error.value().bytes() });
    if (sf_text_or_error.is_error())
        return make_error("META-INF/mozilla.sf is not valid UTF-8"_string);
    if (auto err = verify_mozilla_sf(sf_text_or_error.value(), mf_or_error.value().bytes(), !has_cose); err.is_error())
        return make_error(MUST(String::formatted("Signature manifest verification failed: {}", err.error())));

    auto mf_text_or_error = String::from_utf8(StringView { mf_or_error.value().bytes() });
    if (mf_text_or_error.is_error())
        return make_error("META-INF/manifest.mf is not valid UTF-8"_string);
    auto mf_entries_or_error = parse_manifest_mf(mf_text_or_error.value());
    if (mf_entries_or_error.is_error())
        return make_error(MUST(String::formatted("Invalid META-INF/manifest.mf: {}", mf_entries_or_error.error())));
    auto mf_entries = mf_entries_or_error.release_value();

    HashTable<String> seen_signed_entries;
    HashTable<String> seen_cose_entries;
    for (auto const& entry : zip.entries()) {
        if (entry.is_directory)
            continue;
        if (entry.name == "META-INF/mozilla.rsa"sv || entry.name == "META-INF/mozilla.sf"sv || entry.name == "META-INF/manifest.mf"sv)
            continue;

        auto signed_entry = mf_entries.get(entry.name);
        if (!signed_entry.has_value())
            return make_error(MUST(String::formatted("Unsigned or extra archive entry rejected: {}", entry.name)));

        auto extracted = zip.extract_entry(entry);
        if (extracted.is_error())
            return make_error(MUST(String::formatted("Corrupt archive entry {}: {}", entry.name, extracted.error())));

        if (signed_entry->sha256.has_value()) {
            auto actual = compute_digest(extracted.value().bytes(), EVP_sha256());
            if (actual.is_error() || actual.value().bytes() != signed_entry->sha256->bytes())
                return make_error(MUST(String::formatted("SHA-256 mismatch for signed entry {}", entry.name)));
        } else if (!has_cose) {
            return make_error(MUST(String::formatted("Missing SHA-256 digest for signed entry {}", entry.name)));
        }
        if (signed_entry->sha1.has_value()) {
            auto actual = compute_digest(extracted.value().bytes(), EVP_sha1());
            if (actual.is_error() || actual.value().bytes() != signed_entry->sha1->bytes())
                return make_error(MUST(String::formatted("SHA-1 mismatch for signed entry {}", entry.name)));
        }
        seen_signed_entries.set(entry.name);

        if (entry.name == "META-INF/cose.sig"sv || entry.name == "META-INF/cose.manifest"sv)
            continue;
        if (cose_entries.has_value()) {
            auto cose_entry = cose_entries->get(entry.name);
            if (!cose_entry.has_value() || !cose_entry->sha256.has_value())
                return make_error(MUST(String::formatted("File '{}' is missing a SHA-256 digest in the COSE manifest", entry.name)));
            auto digest = compute_digest(extracted.value().bytes(), EVP_sha256());
            if (digest.is_error() || digest.value().bytes() != cose_entry->sha256->bytes())
                return make_error(MUST(String::formatted("COSE digest mismatch for signed file {}", entry.name)));
            seen_cose_entries.set(entry.name);
        }
        file_names.append(entry.name);
        file_contents.append(extracted.release_value());
    }

    for (auto const& [name, _] : mf_entries) {
        if (!seen_signed_entries.contains(name))
            return make_error(MUST(String::formatted("Signed entry missing from XPI archive: {}", name)));
    }
    if (cose_entries.has_value() && cose_entries->size() != seen_cose_entries.size())
        return make_error("COSE manifest lists absent archive files"_string);

    auto manifest_bytes = zip.extract_entry_by_name("manifest.json"sv);
    if (manifest_bytes.is_error())
        return make_error("XPI missing manifest.json"_string);
    auto manifest_text = String::from_utf8(StringView { manifest_bytes.value().bytes() });
    if (manifest_text.is_error())
        return make_error("manifest.json is not valid UTF-8"_string);

    auto manifest_or_error = Web::Extensions::ExtensionManifest::parse(manifest_text.value());
    if (manifest_or_error.is_error())
        return make_error(MUST(String::formatted("Invalid manifest.json: {}", manifest_or_error.error())));
    auto manifest = manifest_or_error.release_value();

    if (!manifest.gecko_id().has_value() || manifest.gecko_id()->is_empty())
        return make_error("Signed XPI manifest missing browser_specific_settings.gecko.id"_string);
    auto addon_id = *manifest.gecko_id();

    String expected_signer = addon_id;
    if (addon_id.bytes().size() > 64) {
        auto id_hash = sha256_hex_string(addon_id.bytes());
        if (id_hash.is_error())
            return make_error("Could not verify the extension ID digest"_string);
        expected_signer = id_hash.release_value();
    }
    if (cms.signer != expected_signer)
        return make_error(MUST(String::formatted("Signer CN '{}' does not match manifest addon ID '{}'", cms.signer, addon_id)));

    if (expected_addon_id.has_value() && addon_id != *expected_addon_id)
        return make_error(MUST(String::formatted("Expected addon ID '{}' but found '{}'", *expected_addon_id, addon_id)));

    if (revoked_addon_ids.contains_slow(addon_id))
        return make_error(MUST(String::formatted("Extension '{}' is on the revocation list", addon_id)));

    auto compatibility = Web::Extensions::SupportMatrix::evaluate_manifest(manifest, sha256_hex);

    return Web::Extensions::XpiVerificationResult {
        .ok = true,
        .error = {},
        .addon_id = move(addon_id),
        .version = manifest.version(),
        .sha256_hex = move(sha256_hex),
        .manifest_json = manifest.raw_json(),
        .compatibility_status = compatibility.status,
        .compatibility_reasons = move(compatibility.unsupported_reasons),
        .verified_signature = true,
        .signer_cn = move(cms.signer),
    };
}

}
