/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "HeadlessTest.h"
#include <AK/Base64.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Directory.h>
#include <LibCore/StandardPaths.h>
#include <LibCore/System.h>
#include <LibDatabase/Database.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>
#include <LibWebCommon/Extensions/SupportMatrix.h>
#include <LibWebView/Extensions/CatalogClient.h>
#include <LibWebView/Extensions/ExtensionActionController.h>
#include <LibWebView/Extensions/ExtensionSidebarController.h>
#include <LibWebView/Extensions/ExtensionStorage.h>
#include <LibWebView/Extensions/RevocationList.h>
#include <LibWebView/WebUI/AddonsUI.h>
#include <openssl/evp.h>
#include <openssl/pem.h>

using namespace WebView::Extensions;

namespace {

struct SignedEnvelope {
    String envelope_json;
    String public_key_pem;
};

SignedEnvelope sign_json_payload_ed25519(StringView payload_json)
{
    EVP_PKEY* pkey = nullptr;
    EVP_PKEY_CTX* kctx = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    VERIFY(kctx);
    VERIFY(EVP_PKEY_keygen_init(kctx) == 1);
    VERIFY(EVP_PKEY_keygen(kctx, &pkey) == 1);
    EVP_PKEY_CTX_free(kctx);
    ScopeGuard free_pkey = [&] { EVP_PKEY_free(pkey); };

    BIO* pub_bio = BIO_new(BIO_s_mem());
    VERIFY(PEM_write_bio_PUBKEY(pub_bio, pkey) == 1);
    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(pub_bio, &bptr);
    auto pub_pem = MUST(String::from_utf8(StringView { bptr->data, bptr->length }));
    BIO_free(pub_bio);

    EVP_MD_CTX* mctx = EVP_MD_CTX_new();
    VERIFY(EVP_DigestSignInit(mctx, nullptr, nullptr, nullptr, pkey) == 1);
    size_t sig_len = 0;
    VERIFY(EVP_DigestSign(mctx, nullptr, &sig_len, reinterpret_cast<unsigned char const*>(payload_json.bytes().data()), payload_json.bytes().size()) == 1);
    auto sig_buf = MUST(ByteBuffer::create_uninitialized(sig_len));
    VERIFY(EVP_DigestSign(mctx, sig_buf.data(), &sig_len, reinterpret_cast<unsigned char const*>(payload_json.bytes().data()), payload_json.bytes().size()) == 1);
    EVP_MD_CTX_free(mctx);
    sig_buf.resize(sig_len);

    JsonObject env;
    env.set("payload"sv, MUST(String::from_utf8(payload_json)));
    env.set("signature"sv, MUST(encode_base64(sig_buf.bytes())));
    return { env.serialized(), move(pub_pem) };
}

}

TEST_CASE(extension_storage_sqlite_persistence_and_changes)
{
    auto tmp_dir = ByteString::formatted("{}/ladyflock-storage-test-{}", Core::StandardPaths::tempfile_directory(), Core::System::getpid());
    MUST(Core::Directory::create(tmp_dir, Core::Directory::CreateDirectories::Yes));
    ScopeGuard cleanup = [&] {
        if (FileSystem::exists(tmp_dir))
            (void)FileSystem::remove(tmp_dir, FileSystem::RecursionMode::Allowed);
    };

    {
        auto db = TRY_OR_FAIL(Database::Database::create(tmp_dir, "ExtStorageTest"sv));
        EXPECT_EQ(TRY_OR_FAIL(ExtensionStorage::migrate_schema(db)), Database::MigrationOutcome::Success);
        auto storage = TRY_OR_FAIL(ExtensionStorage::create(db));

        JsonObject initial;
        initial.set("theme"sv, "dark"_string);
        initial.set("count"sv, 1);
        auto changes = storage->set("ext@a"sv, initial);
        EXPECT(changes.has("theme"sv));
        EXPECT_EQ(changes.get_object("theme"sv)->get_string("newValue"sv), Optional<String> { "dark"_string });
        EXPECT(!changes.get_object("theme"sv)->has("oldValue"sv));

        JsonObject update;
        update.set("count"sv, 2);
        auto update_changes = storage->set("ext@a"sv, update);
        EXPECT_EQ(update_changes.get_object("count"sv)->get_i64("oldValue"sv), Optional<i64> { 1 });
        EXPECT_EQ(update_changes.get_object("count"sv)->get_i64("newValue"sv), Optional<i64> { 2 });

        JsonObject defaults;
        defaults.set("missing"sv, "fallback"_string);
        defaults.set("theme"sv, "light"_string);
        auto got = storage->get("ext@a"sv, JsonValue(defaults));
        EXPECT_EQ(got.get_string("theme"sv), Optional<String> { "dark"_string });
        EXPECT_EQ(got.get_string("missing"sv), Optional<String> { "fallback"_string });
    }

    // Reopen database and confirm persistence + remove + clear.
    {
        auto db = TRY_OR_FAIL(Database::Database::create(tmp_dir, "ExtStorageTest"sv));
        auto storage = TRY_OR_FAIL(ExtensionStorage::create(db));

        auto got = storage->get("ext@a"sv, JsonValue {});
        EXPECT_EQ(got.get_string("theme"sv), Optional<String> { "dark"_string });
        EXPECT_EQ(got.get_i64("count"sv), Optional<i64> { 2 });

        auto removed = storage->remove("ext@a"sv, { "theme"_string });
        EXPECT_EQ(removed.get_object("theme"sv)->get_string("oldValue"sv), Optional<String> { "dark"_string });
        EXPECT(!storage->get("ext@a"sv, JsonValue {}).has("theme"sv));

        auto cleared = storage->clear("ext@a"sv);
        EXPECT(cleared.has("count"sv));
        EXPECT(storage->get("ext@a"sv, JsonValue {}).is_empty());
    }
}

TEST_CASE(extension_action_controller_precedence_and_invoke)
{
    auto manifest = Web::Extensions::ExtensionManifest::parse(R"({
        "manifest_version": 2,
        "name": "Action Test",
        "version": "1.0",
        "browser_action": {
            "default_title": "Global Title",
            "default_popup": "popup.html"
        }
    })"sv)
                        .release_value();

    ExtensionActionController ctrl;
    ctrl.configure_from_manifest("act@test"_string, manifest);
    EXPECT_EQ(ctrl.get_title("act@test"sv), "Global Title"_string);
    EXPECT_EQ(ctrl.get_popup("act@test"sv, 10, 1), Optional<String> { "popup.html"_string });

    // Window overrides global; tab overrides window.
    ctrl.set_title("act@test"_string, "Window Title"_string, {}, 1);
    EXPECT_EQ(ctrl.get_title("act@test"sv, 10, 1), "Window Title"_string);
    ctrl.set_title("act@test"_string, "Tab Title"_string, 10, 1);
    EXPECT_EQ(ctrl.get_title("act@test"sv, 10, 1), "Tab Title"_string);
    EXPECT_EQ(ctrl.get_title("act@test"sv, 11, 1), "Window Title"_string);

    ctrl.set_badge_text("act@test"_string, "5"_string, 10, 1);
    EXPECT_EQ(ctrl.get_badge_text("act@test"sv, 10, 1), "5"_string);

    bool popup_opened = false;
    auto outcome = ctrl.invoke_action(
        "act@test"sv,
        10,
        1,
        [&](Optional<String> const& path) { popup_opened = (path == Optional<String> { "popup.html"_string }); },
        []() { VERIFY_NOT_REACHED(); });
    EXPECT_EQ(outcome, ExtensionActionController::InvokeOutcome::OpenedPopup);
    EXPECT(popup_opened);

    // Clearing popup switches invocation to DispatchedOnClicked.
    ctrl.set_popup("act@test"_string, ""_string, 10, 1);
    bool clicked = false;
    outcome = ctrl.invoke_action(
        "act@test"sv,
        10,
        1,
        [](Optional<String> const&) { VERIFY_NOT_REACHED(); },
        [&]() { clicked = true; });
    EXPECT_EQ(outcome, ExtensionActionController::InvokeOutcome::DispatchedOnClicked);
    EXPECT(clicked);
}

TEST_CASE(extension_sidebar_controller_precedence_gesture_and_width)
{
    auto manifest = Web::Extensions::ExtensionManifest::parse(R"({
        "manifest_version": 2,
        "name": "Sidebar Test",
        "version": "1.0",
        "sidebar_action": {
            "default_panel": "panel.html",
            "default_title": "Sidebar",
            "open_at_install": true
        }
    })"sv)
                        .release_value();

    ExtensionSidebarController sidebar;
    sidebar.configure_from_manifest("side@test"_string, manifest, true, 1);
    EXPECT(sidebar.is_open("side@test"sv, 1));

    sidebar.close("side@test"sv, 1);
    EXPECT(!sidebar.is_open("side@test"sv, 1));

    // Programmatic open/toggle requires user gesture.
    EXPECT(sidebar.open("side@test"_string, 1, false).is_error());
    EXPECT(sidebar.toggle("side@test"_string, 1, false).is_error());
    EXPECT(!sidebar.is_open("side@test"sv, 1));

    MUST(sidebar.open("side@test"_string, 1, true));
    EXPECT(sidebar.is_open("side@test"sv, 1));
    EXPECT(!MUST(sidebar.toggle("side@test"_string, 1, true)));
    EXPECT(!sidebar.is_open("side@test"sv, 1));

    // Precedence tab > window > global.
    sidebar.set_panel("side@test"_string, "win.html"_string, {}, 1);
    sidebar.set_panel("side@test"_string, "tab.html"_string, 42, 1);
    EXPECT_EQ(sidebar.get_panel("side@test"sv, 42, 1), Optional<String> { "tab.html"_string });
    EXPECT_EQ(sidebar.get_panel("side@test"sv, 43, 1), Optional<String> { "win.html"_string });
    EXPECT_EQ(sidebar.get_panel("side@test"sv, {}, {}), Optional<String> { "panel.html"_string });

    // Persisted width clamping.
    sidebar.set_width(1, 50);
    EXPECT_EQ(sidebar.window_state(1).width_px, ExtensionSidebarController::min_width_px);
    sidebar.set_width(1, 900);
    EXPECT_EQ(sidebar.window_state(1).width_px, ExtensionSidebarController::max_width_px);
    sidebar.set_width(1, 410);
    EXPECT_EQ(sidebar.persisted_default_width(), 410u);
}

TEST_CASE(catalog_client_gating_and_offline_amo_fixture)
{
    CatalogClient client;
    EXPECT(!client.enable_public_amo_catalog());

    bool saw_error = false;
    client.search("quicknote"sv, [&](ErrorOr<Vector<CatalogEntry>> res) {
        saw_error = res.is_error();
    });
    EXPECT(saw_error);

    auto fixture_json = R"({
        "results": [
            {
                "guid": "quicknote@ladyflock.example",
                "slug": "quicknote",
                "name": {"en-US": "Quicknote"},
                "summary": {"en-US": "Simple sidebar note"},
                "current_version": {
                    "version": "1.0.0",
                    "file": {
                        "url": "https://example.invalid/quicknote.xpi",
                        "hash": "sha256:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
                        "permissions": ["storage"]
                    }
                }
            },
            {
                "guid": "bad@ext",
                "slug": "bad-ext",
                "name": "Incompatible Ext",
                "current_version": {
                    "version": "1.0",
                    "file": {
                        "url": "https://example.invalid/bad.xpi",
                        "hash": "sha256:0000",
                        "permissions": ["webNavigation"]
                    }
                }
            }
        ]
    })"_string;

    client.set_offline_fixture_json(fixture_json);
    Vector<CatalogEntry> entries;
    client.search("quicknote"sv, [&](ErrorOr<Vector<CatalogEntry>> res) {
        VERIFY(!res.is_error());
        entries = res.release_value();
    });
    EXPECT_EQ(entries.size(), 2u);
    EXPECT_EQ(entries[0].compatibility_status, Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested);
    EXPECT(entries[0].compatibility_reasons.is_empty());
    EXPECT_EQ(entries[1].compatibility_status, Web::Extensions::CompatibilityStatus::Incompatible);
    EXPECT(!entries[1].compatibility_reasons.is_empty());
}

TEST_CASE(revocation_list_signed_verification_and_matching)
{
    auto payload = R"({
        "entries": [
            { "addon_id": "bad@ext", "version": "1.0", "reason": "Malicious" },
            { "sha256": "deadbeef0123", "reason": "Compromised build" }
        ]
    })"sv;
    auto signed_env = sign_json_payload_ed25519(payload);

    RevocationList list;
    TRY_OR_FAIL(list.load_signed_envelope(signed_env.envelope_json, signed_env.public_key_pem));
    EXPECT(list.is_revoked("bad@ext"sv, "1.0"sv, {}));
    EXPECT(!list.is_revoked("bad@ext"sv, "2.0"sv, {}));
    EXPECT(list.is_revoked("other@ext"sv, "1.0"sv, "deadbeef0123"sv));

    // Tampered envelope must fail closed and leave previous list intact.
    auto tampered = MUST(signed_env.envelope_json.replace("Malicious"sv, "Benign"sv, ReplaceMode::All));
    RevocationList strict_list;
    EXPECT(strict_list.load_signed_envelope(tampered, signed_env.public_key_pem).is_error());
    EXPECT(strict_list.entries().is_empty());
}

TEST_CASE(addons_ui_backend_commands)
{
    Test::HeadlessTestEnvironment environment { "TestAddonsUI"sv };
    auto app = TRY_OR_FAIL(environment.create_application(Array { "--enable-extensions"sv, "--temporary-profile"sv }.span()));

    auto matrix_val = WebView::AddonsUI::handle_command_for_testing("getSupportMatrix"sv, {});
    EXPECT(matrix_val.is_object());
    EXPECT(matrix_val.as_object().has("manifest_keys"sv));
    EXPECT(matrix_val.as_object().has("permissions"sv));
    EXPECT(matrix_val.as_object().has("api_methods"sv));

    auto installed_val = WebView::AddonsUI::handle_command_for_testing("getInstalledExtensions"sv, {});
    EXPECT(installed_val.is_object());
    EXPECT(installed_val.as_object().has("extensions"sv));
}
