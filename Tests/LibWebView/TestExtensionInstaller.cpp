/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/Base64.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <AK/StringBuilder.h>
#include <LibCore/Directory.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/Application.h>
#include <LibWebView/BrowsingSession.h>
#include <LibWebView/Extensions/ExtensionManager.h>
#include <openssl/bio.h>
#include <openssl/cms.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <zlib.h>

namespace {

class InstallerTestApp : public Test::HeadlessApplication {
    WEB_VIEW_APPLICATION(InstallerTestApp)

public:
    explicit InstallerTestApp(Optional<ByteString> ladybird_binary_path)
        : Test::HeadlessApplication(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions& browser_options, WebView::RequestServerOptions&, WebView::WebContentOptions&) override
    {
        browser_options.headless_mode = WebView::HeadlessMode::Test;
        browser_options.disable_sql_database = WebView::DisableSQLDatabase::No;
        browser_options.enable_extensions = true;
    }
};

struct ZipEntryInput {
    ByteString name;
    ByteBuffer bytes;
};

void append_u16(ByteBuffer& buf, u16 val)
{
    u8 b[2] = { static_cast<u8>(val & 0xff), static_cast<u8>((val >> 8) & 0xff) };
    MUST(buf.try_append(b, 2));
}

void append_u32(ByteBuffer& buf, u32 val)
{
    u8 b[4] = {
        static_cast<u8>(val & 0xff),
        static_cast<u8>((val >> 8) & 0xff),
        static_cast<u8>((val >> 16) & 0xff),
        static_cast<u8>((val >> 24) & 0xff),
    };
    MUST(buf.try_append(b, 4));
}

ByteBuffer build_zip_bytes(Vector<ZipEntryInput> const& entries)
{
    ByteBuffer out;
    struct Rec {
        ByteString name;
        u32 crc32 { 0 };
        u32 size { 0 };
        u32 offset { 0 };
    };
    Vector<Rec> recs;

    for (auto const& e : entries) {
        u32 offset = static_cast<u32>(out.size());
        u32 crc = static_cast<u32>(::crc32(0L, e.bytes.bytes().data(), static_cast<uInt>(e.bytes.bytes().size())));
        u32 size = static_cast<u32>(e.bytes.size());

        append_u32(out, 0x04034b50u);
        append_u16(out, 20);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u32(out, crc);
        append_u32(out, size);
        append_u32(out, size);
        append_u16(out, static_cast<u16>(e.name.length()));
        append_u16(out, 0);
        MUST(out.try_append(e.name.bytes()));
        MUST(out.try_append(e.bytes.bytes()));

        recs.append({ e.name, crc, size, offset });
    }

    u32 cd_offset = static_cast<u32>(out.size());
    for (auto const& r : recs) {
        append_u32(out, 0x02014b50u);
        append_u16(out, 20);
        append_u16(out, 20);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u32(out, r.crc32);
        append_u32(out, r.size);
        append_u32(out, r.size);
        append_u16(out, static_cast<u16>(r.name.length()));
        append_u16(out, 0);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u32(out, 0);
        append_u32(out, r.offset);
        MUST(out.try_append(r.name.bytes()));
    }
    u32 cd_size = static_cast<u32>(out.size()) - cd_offset;

    append_u32(out, 0x06054b50u);
    append_u16(out, 0);
    append_u16(out, 0);
    append_u16(out, static_cast<u16>(recs.size()));
    append_u16(out, static_cast<u16>(recs.size()));
    append_u32(out, cd_size);
    append_u32(out, cd_offset);
    append_u16(out, 0);
    return out;
}

ByteBuffer sha256_raw(ReadonlyBytes bytes)
{
    auto* ctx = EVP_MD_CTX_new();
    VERIFY(ctx);
    ScopeGuard free_ctx = [&] { EVP_MD_CTX_free(ctx); };
    VERIFY(EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) == 1);
    if (!bytes.is_empty())
        VERIFY(EVP_DigestUpdate(ctx, bytes.data(), bytes.size()) == 1);
    unsigned int len = 0;
    auto out = MUST(ByteBuffer::create_uninitialized(EVP_MAX_MD_SIZE));
    VERIFY(EVP_DigestFinal_ex(ctx, out.data(), &len) == 1);
    out.resize(len);
    return out;
}

void add_x509_ext(X509* issuer, X509* subject, int nid, char const* value)
{
    X509V3_CTX ctx {};
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer, subject, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, const_cast<char*>(value));
    VERIFY(ext);
    VERIFY(X509_add_ext(subject, ext, -1) == 1);
    X509_EXTENSION_free(ext);
}

EVP_PKEY* generate_rsa_key()
{
    EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    VERIFY(kctx);
    VERIFY(EVP_PKEY_keygen_init(kctx) == 1);
    VERIFY(EVP_PKEY_CTX_set_rsa_keygen_bits(kctx, 2048) == 1);
    EVP_PKEY* pkey = nullptr;
    VERIFY(EVP_PKEY_keygen(kctx, &pkey) == 1);
    EVP_PKEY_CTX_free(kctx);
    return pkey;
}

struct EphemeralSigningAuthority {
    EVP_PKEY* ca_key { nullptr };
    X509* ca_cert { nullptr };
    String ca_pem;

    EphemeralSigningAuthority()
    {
        ca_key = generate_rsa_key();
        ca_cert = X509_new();
        VERIFY(X509_set_version(ca_cert, 2) == 1);
        ASN1_INTEGER_set(X509_get_serialNumber(ca_cert), 1);
        X509_gmtime_adj(X509_getm_notBefore(ca_cert), -60);
        X509_gmtime_adj(X509_getm_notAfter(ca_cert), 86400);
        VERIFY(X509_set_pubkey(ca_cert, ca_key) == 1);

        X509_NAME* name = X509_get_subject_name(ca_cert);
        VERIFY(X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<unsigned char const*>("Ladyflock Test Root CA"), -1, -1, 0) == 1);
        VERIFY(X509_set_issuer_name(ca_cert, name) == 1);

        add_x509_ext(ca_cert, ca_cert, NID_basic_constraints, "critical,CA:TRUE");
        add_x509_ext(ca_cert, ca_cert, NID_key_usage, "critical,keyCertSign,cRLSign");
        add_x509_ext(ca_cert, ca_cert, NID_subject_key_identifier, "hash");

        VERIFY(X509_sign(ca_cert, ca_key, EVP_sha256()) > 0);

        BIO* bio = BIO_new(BIO_s_mem());
        VERIFY(PEM_write_bio_X509(bio, ca_cert) == 1);
        BUF_MEM* bptr = nullptr;
        BIO_get_mem_ptr(bio, &bptr);
        ca_pem = MUST(String::from_utf8(StringView { bptr->data, bptr->length }));
        BIO_free(bio);
    }

    ~EphemeralSigningAuthority()
    {
        X509_free(ca_cert);
        EVP_PKEY_free(ca_key);
    }

    ByteBuffer build_signed_xpi(
        Vector<ZipEntryInput> const& content_entries,
        StringView signer_cn,
        Optional<ZipEntryInput> extra_unsigned_entry = {},
        Optional<ByteString> tamper_entry_name = {}) const
    {
        // Build manifest.mf over content_entries
        StringBuilder mf;
        mf.append("Manifest-Version: 1.0\r\n\r\n"sv);
        for (auto const& e : content_entries) {
            auto digest = sha256_raw(e.bytes.bytes());
            auto b64 = MUST(encode_base64(digest.bytes()));
            mf.appendff("Name: {}\r\nSHA256-Digest: {}\r\n\r\n", e.name, b64);
        }
        auto mf_bytes = MUST(ByteBuffer::copy(mf.string_view().bytes()));

        auto mf_digest = sha256_raw(mf_bytes.bytes());
        auto sf_text = MUST(String::formatted("Signature-Version: 1.0\r\nSHA256-Digest-Manifest: {}\r\n\r\n", MUST(encode_base64(mf_digest.bytes()))));
        auto sf_bytes = MUST(ByteBuffer::copy(sf_text.bytes()));

        // Issue leaf cert with CN = signer_cn
        EVP_PKEY* leaf_key = generate_rsa_key();
        ScopeGuard free_leaf_key = [&] { EVP_PKEY_free(leaf_key); };
        X509* leaf = X509_new();
        ScopeGuard free_leaf = [&] { X509_free(leaf); };

        VERIFY(X509_set_version(leaf, 2) == 1);
        ASN1_INTEGER_set(X509_get_serialNumber(leaf), 2);
        X509_gmtime_adj(X509_getm_notBefore(leaf), -60);
        X509_gmtime_adj(X509_getm_notAfter(leaf), 86400);
        VERIFY(X509_set_pubkey(leaf, leaf_key) == 1);

        X509_NAME* subj = X509_get_subject_name(leaf);
        ByteString cn_bs { signer_cn };
        VERIFY(X509_NAME_add_entry_by_txt(subj, "CN", MBSTRING_UTF8, reinterpret_cast<unsigned char const*>(cn_bs.characters()), static_cast<int>(cn_bs.length()), -1, 0) == 1);
        VERIFY(X509_set_issuer_name(leaf, X509_get_subject_name(ca_cert)) == 1);

        add_x509_ext(ca_cert, leaf, NID_basic_constraints, "critical,CA:FALSE");
        add_x509_ext(ca_cert, leaf, NID_key_usage, "critical,digitalSignature");
        add_x509_ext(ca_cert, leaf, NID_ext_key_usage, "codeSigning");
        add_x509_ext(ca_cert, leaf, NID_subject_key_identifier, "hash");
        add_x509_ext(ca_cert, leaf, NID_authority_key_identifier, "keyid:always");

        VERIFY(X509_sign(leaf, ca_key, EVP_sha256()) > 0);

        BIO* sf_bio = BIO_new_mem_buf(sf_bytes.data(), static_cast<int>(sf_bytes.size()));
        CMS_ContentInfo* cms = CMS_sign(leaf, leaf_key, nullptr, sf_bio, CMS_DETACHED | CMS_BINARY | CMS_NOSMIMECAP);
        BIO_free(sf_bio);
        VERIFY(cms);
        ScopeGuard free_cms = [&] { CMS_ContentInfo_free(cms); };

        BIO* rsa_bio = BIO_new(BIO_s_mem());
        VERIFY(i2d_CMS_bio(rsa_bio, cms) == 1);
        BUF_MEM* rsa_mem = nullptr;
        BIO_get_mem_ptr(rsa_bio, &rsa_mem);
        auto rsa_bytes = MUST(ByteBuffer::copy(ReadonlyBytes { rsa_mem->data, rsa_mem->length }));
        BIO_free(rsa_bio);

        Vector<ZipEntryInput> archive_entries;
        archive_entries.append({ "META-INF/manifest.mf", mf_bytes });
        archive_entries.append({ "META-INF/mozilla.sf", sf_bytes });
        archive_entries.append({ "META-INF/mozilla.rsa", rsa_bytes });
        for (auto const& e : content_entries) {
            if (tamper_entry_name.has_value() && e.name == *tamper_entry_name)
                archive_entries.append({ e.name, MUST(ByteBuffer::copy("/* TAMPERED */"sv.bytes())) });
            else
                archive_entries.append(e);
        }
        if (extra_unsigned_entry.has_value())
            archive_entries.append(*extra_unsigned_entry);

        return build_zip_bytes(archive_entries);
    }
};

ByteString write_temp_file(ByteString const& path, ReadonlyBytes bytes)
{
    auto f = MUST(Core::File::open(path, Core::File::OpenMode::Write));
    MUST(f->write_until_depleted(bytes));
    return path;
}

}

TEST_CASE(signed_xpi_sandboxed_verification_install_update_and_isolation)
{
    Test::HeadlessTestEnvironment env { "TestExtensionInstaller"sv, 90 };
    MUST(Core::Environment::set("XDG_DATA_HOME"sv, env.directory(), Core::Environment::Overwrite::Yes));
    auto app = TRY_OR_FAIL(env.create_application<InstallerTestApp>(Array { "--temporary-profile"sv }.span()));
    auto& mgr = *app->default_session().extension_manager;

    EphemeralSigningAuthority ca;
    mgr.installer().add_trusted_root_pem(ca.ca_pem);

    auto work_dir = LexicalPath::join(env.directory(), "xpis"sv).string();
    MUST(Core::Directory::create(work_dir, Core::Directory::CreateDirectories::Yes));

    StringView addon_id = "signed-test@ladyflock.example"sv;
    auto manifest_v1 = MUST(ByteBuffer::copy(R"({
        "manifest_version": 2,
        "name": "Signed Test",
        "version": "1.0.0",
        "browser_specific_settings": { "gecko": { "id": "signed-test@ladyflock.example" } },
        "permissions": ["storage"]
    })"sv.bytes()));
    auto script_v1 = MUST(ByteBuffer::copy("console.log('v1');"sv.bytes()));

    // 1. Valid signed XPI staging + cancellation cleans staging dir
    auto valid_v1_xpi = ca.build_signed_xpi({ { "manifest.json", manifest_v1 }, { "bg.js", script_v1 } }, addon_id);
    auto valid_v1_path = write_temp_file(
        LexicalPath::join(work_dir, "valid-v1.xpi"sv).string(),
        valid_v1_xpi.bytes());

    auto staged_cancel = TRY_OR_FAIL(mgr.stage_xpi(valid_v1_path, String::from_utf8_without_validation(addon_id.bytes())));
    EXPECT(FileSystem::exists(staged_cancel.staged_directory));
    TRY_OR_FAIL(mgr.cancel_install(staged_cancel.token));
    EXPECT(!FileSystem::exists(staged_cancel.staged_directory));
    EXPECT(!mgr.extension_by_id(addon_id).has_value());

    // 2. Tampered entry, unsigned extra entry, CN mismatch, and revoked ID must fail closed
    auto tampered_xpi = ca.build_signed_xpi({ { "manifest.json", manifest_v1 }, { "bg.js", script_v1 } }, addon_id, {}, ByteString("bg.js"sv));
    auto tampered_path = write_temp_file(
        LexicalPath::join(work_dir, "tampered.xpi"sv).string(),
        tampered_xpi.bytes());
    EXPECT(mgr.stage_xpi(tampered_path).is_error());

    auto unsigned_extra_xpi = ca.build_signed_xpi({ { "manifest.json", manifest_v1 }, { "bg.js", script_v1 } }, addon_id, ZipEntryInput { "evil.js", script_v1 });
    auto unsigned_extra_path = write_temp_file(
        LexicalPath::join(work_dir, "extra.xpi"sv).string(),
        unsigned_extra_xpi.bytes());
    EXPECT(mgr.stage_xpi(unsigned_extra_path).is_error());

    auto cn_mismatch_xpi = ca.build_signed_xpi({ { "manifest.json", manifest_v1 }, { "bg.js", script_v1 } }, "wrong@other.example"sv);
    auto cn_mismatch_path = write_temp_file(
        LexicalPath::join(work_dir, "wrong-cn.xpi"sv).string(),
        cn_mismatch_xpi.bytes());
    EXPECT(mgr.stage_xpi(cn_mismatch_path).is_error());

    // 3. Approve valid v1 install and write storage.local data
    auto staged_v1 = TRY_OR_FAIL(mgr.stage_xpi(valid_v1_path));
    EXPECT(staged_v1.verified_signature);
    auto const& installed_v1 = TRY_OR_FAIL(mgr.approve_install(staged_v1.token));
    String profile1_uuid = installed_v1.uuid;
    ByteString v1_dir = installed_v1.install_directory;
    EXPECT_EQ(installed_v1.manifest.version(), "1.0.0"_string);

    JsonObject stored;
    stored.set("note"sv, "keep-across-update"_string);
    (void)mgr.storage().set(addon_id, stored);

    // 4. Update v2 adds "tabs" permission -> requires_permission_prompt == true; rejecting keeps v1
    auto manifest_v2 = MUST(ByteBuffer::copy(R"({
        "manifest_version": 2,
        "name": "Signed Test",
        "version": "2.0.0",
        "browser_specific_settings": { "gecko": { "id": "signed-test@ladyflock.example" } },
        "permissions": ["storage", "tabs"]
    })"sv.bytes()));
    auto script_v2 = MUST(ByteBuffer::copy("console.log('v2');"sv.bytes()));
    auto valid_v2_xpi = ca.build_signed_xpi({ { "manifest.json", manifest_v2 }, { "bg.js", script_v2 } }, addon_id);
    auto valid_v2_path = write_temp_file(
        LexicalPath::join(work_dir, "valid-v2.xpi"sv).string(),
        valid_v2_xpi.bytes());

    auto staged_v2 = TRY_OR_FAIL(mgr.stage_update(addon_id, valid_v2_path));
    EXPECT(staged_v2.requires_permission_prompt);
    EXPECT(staged_v2.newly_requested_permissions.contains_slow("tabs"_string));
    EXPECT(mgr.approve_update(staged_v2.token, false).is_error());
    EXPECT_EQ(mgr.extension_by_id(addon_id)->manifest.version(), "1.0.0"_string);

    // 5. Simulated commit failure rolls back to v1 cleanly
    auto staged_v2_retry = TRY_OR_FAIL(mgr.stage_update(addon_id, valid_v2_path));
    mgr.installer().set_fail_next_commit_for_testing(true);
    EXPECT(mgr.approve_update(staged_v2_retry.token, true).is_error());
    EXPECT(FileSystem::exists(LexicalPath::join(v1_dir, "manifest.json"sv).string()));
    EXPECT_EQ(mgr.extension_by_id(addon_id)->manifest.version(), "1.0.0"_string);
    EXPECT_EQ(mgr.storage().get(addon_id, JsonValue {}).get_string("note"sv), Optional<String> { "keep-across-update"_string });

    // 6. Successful update v2 preserves UUID and storage.local
    auto staged_v2_ok = TRY_OR_FAIL(mgr.stage_update(addon_id, valid_v2_path));
    auto const& installed_v2 = TRY_OR_FAIL(mgr.approve_update(staged_v2_ok.token, true));
    EXPECT_EQ(installed_v2.manifest.version(), "2.0.0"_string);
    EXPECT_EQ(installed_v2.uuid, profile1_uuid);
    EXPECT_EQ(mgr.storage().get(addon_id, JsonValue {}).get_string("note"sv), Optional<String> { "keep-across-update"_string });
    EXPECT(mgr.stage_update(addon_id, valid_v1_path).is_error());
    EXPECT(mgr.stage_xpi(valid_v1_path).is_error());
    EXPECT(mgr.approve_install(staged_v2.token).is_error());
    EXPECT(mgr.approve_update(staged_v2.token, true).is_error());
    TRY_OR_FAIL(mgr.cancel_install(staged_v2.token));

    // 7. Revocation check blocks staging by addon_id and by sha256
    auto rev_manifest = MUST(ByteBuffer::copy(R"({
        "manifest_version": 2,
        "name": "Revoked",
        "version": "1.0",
        "browser_specific_settings": { "gecko": { "id": "revoked@ext" } }
    })"sv.bytes()));
    auto rev_xpi = ca.build_signed_xpi({ { "manifest.json", rev_manifest } }, "revoked@ext"sv);
    auto rev_path = write_temp_file(
        LexicalPath::join(work_dir, "revoked.xpi"sv).string(),
        rev_xpi.bytes());
    auto rev_staged = TRY_OR_FAIL(mgr.stage_xpi(rev_path));
    mgr.revocation_list().add_entry_for_testing({ .addon_id = "revoked@ext"_string, .version = {}, .sha256_hex = {}, .reason = "Test"_string });
    EXPECT(mgr.approve_install(rev_staged.token).is_error());
    TRY_OR_FAIL(mgr.cancel_install(rev_staged.token));
    EXPECT(mgr.stage_xpi(rev_path).is_error());

    // 8. Second profile has independent UUID and empty storage.local
    auto profile2_dir = LexicalPath::join(env.directory(), "profile2"sv).string();
    MUST(Core::Directory::create(profile2_dir, Core::Directory::CreateDirectories::Yes));
    auto mgr2 = TRY_OR_FAIL(WebView::Extensions::ExtensionManager::create(profile2_dir, nullptr));
    mgr2->set_extensions_enabled(true);
    mgr2->installer().add_trusted_root_pem(ca.ca_pem);
    auto staged_p2 = TRY_OR_FAIL(mgr2->stage_xpi(valid_v1_path));
    auto const& installed_p2 = TRY_OR_FAIL(mgr2->approve_install(staged_p2.token));
    EXPECT_NE(installed_p2.uuid, profile1_uuid);
    EXPECT(mgr2->storage().get(addon_id, JsonValue {}).is_empty());

    auto reopened = TRY_OR_FAIL(WebView::Extensions::ExtensionManager::create(profile2_dir, nullptr));
    reopened->set_extensions_enabled(true);
    reopened->installer().add_trusted_root_pem(ca.ca_pem);
    TRY_OR_FAIL(reopened->initialize_persisted_extensions());
    auto restored = reopened->extension_by_id(addon_id);
    EXPECT(restored.has_value());
    if (restored.has_value())
        EXPECT_EQ(restored->uuid, installed_p2.uuid);

    write_temp_file(LexicalPath::join(installed_p2.install_directory, "bg.js"sv).string(), "changed"sv.bytes());
    auto reopened_after_change = TRY_OR_FAIL(WebView::Extensions::ExtensionManager::create(profile2_dir, nullptr));
    reopened_after_change->set_extensions_enabled(true);
    reopened_after_change->installer().add_trusted_root_pem(ca.ca_pem);
    TRY_OR_FAIL(reopened_after_change->initialize_persisted_extensions());
    EXPECT(!reopened_after_change->extension_by_id(addon_id).has_value());

    // A separately downloaded AMO fixture can exercise the actual Mozilla
    // production trust root without adding a network dependency to the suite.
    if (auto public_xpi = Core::Environment::get("LADYFLOCK_TEST_AMO_SCROLL_TO_TOP_XPI"sv); public_xpi.has_value()) {
        auto public_stage = TRY_OR_FAIL(mgr.stage_xpi(*public_xpi));
        EXPECT_EQ(public_stage.addon_id, "scrolltotop-lite@pratikabu"_string);
        EXPECT_EQ(public_stage.version, "1.0"_string);
        EXPECT_EQ(public_stage.sha256_hex, "3641b7d768842222e4b0c8dfc391504e53a5512a6b067406ba110bc3d7f9c97e"_string);
        EXPECT(public_stage.verified_signature);
        auto public_install = TRY_OR_FAIL(mgr.approve_install(public_stage.token));
        EXPECT(FileSystem::exists(LexicalPath::join(public_install.install_directory, "background.js"sv).string()));
    }
}
