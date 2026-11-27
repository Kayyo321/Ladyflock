/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/Endian.h>
#include <AK/ScopeGuard.h>
#include <LibCompress/Zlib.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Script.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/ExtensionWorld.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Extensions/ExtensionContentScriptHost.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/HTMLDocument.h>
#include <LibWeb/HTML/LocalTraversableNavigable.h>
#include <LibWeb/HTML/Scripting/WindowEnvironmentSettingsObject.h>
#include <LibWeb/HTML/Scripting/WindowRealm.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>
#include <LibWebCommon/Extensions/MatchPattern.h>
#include <LibWebCommon/Extensions/NavigationURLFilter.h>
#include <LibWebCommon/Extensions/SupportMatrix.h>
#include <LibWebCommon/Extensions/ZipReader.h>
#include <LibWebCommon/SecureContexts/AbstractOperations.h>
#include <zlib.h>

namespace {

struct ZipFixtureEntry {
    ByteString name;
    ByteBuffer data;
    bool deflate { false };
    u32 override_crc32 { 0 };
    bool has_override_crc32 { false };
    u32 override_uncompressed_size { 0 };
    bool has_override_uncompressed_size { false };
    u32 external_attributes { 0 };
};

void append_u16(ByteBuffer& buf, u16 val)
{
    u8 bytes[2] = { static_cast<u8>(val & 0xff), static_cast<u8>((val >> 8) & 0xff) };
    MUST(buf.try_append(bytes, 2));
}

void append_u32(ByteBuffer& buf, u32 val)
{
    u8 bytes[4] = {
        static_cast<u8>(val & 0xff),
        static_cast<u8>((val >> 8) & 0xff),
        static_cast<u8>((val >> 16) & 0xff),
        static_cast<u8>((val >> 24) & 0xff),
    };
    MUST(buf.try_append(bytes, 4));
}

ByteBuffer deflate_raw_bytes(ReadonlyBytes input)
{
    z_stream zs {};
    VERIFY(deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) == Z_OK);
    ScopeGuard end_zs = [&] { deflateEnd(&zs); };

    auto out = MUST(ByteBuffer::create_uninitialized(deflateBound(&zs, static_cast<uLong>(input.size()))));
    zs.next_in = const_cast<Bytef*>(input.data());
    zs.avail_in = static_cast<uInt>(input.size());
    zs.next_out = out.data();
    zs.avail_out = static_cast<uInt>(out.size());

    int rc = deflate(&zs, Z_FINISH);
    VERIFY(rc == Z_STREAM_END);
    out.resize(static_cast<size_t>(zs.total_out));
    return out;
}

ByteBuffer build_test_zip(Vector<ZipFixtureEntry> const& entries)
{
    ByteBuffer out;
    struct CentralRecord {
        ByteString name;
        u16 method { 0 };
        u32 crc32 { 0 };
        u32 comp_size { 0 };
        u32 uncomp_size { 0 };
        u32 local_offset { 0 };
        u32 external_attrs { 0 };
    };
    Vector<CentralRecord> records;

    for (auto const& e : entries) {
        u32 local_offset = static_cast<u32>(out.size());
        u32 crc = e.has_override_crc32 ? e.override_crc32 : static_cast<u32>(::crc32(0L, e.data.bytes().data(), static_cast<uInt>(e.data.bytes().size())));
        u32 uncomp_size = e.has_override_uncompressed_size ? e.override_uncompressed_size : static_cast<u32>(e.data.size());
        ByteBuffer payload = e.data;
        u16 method = 0;
        if (e.deflate) {
            method = 8;
            payload = deflate_raw_bytes(e.data.bytes());
        }
        u32 comp_size = static_cast<u32>(payload.size());

        // Local file header
        append_u32(out, 0x04034b50u);
        append_u16(out, 20);
        append_u16(out, 0);
        append_u16(out, method);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u32(out, crc);
        append_u32(out, comp_size);
        append_u32(out, uncomp_size);
        append_u16(out, static_cast<u16>(e.name.length()));
        append_u16(out, 0);
        MUST(out.try_append(e.name.bytes()));
        MUST(out.try_append(payload.bytes()));

        records.append({ e.name, method, crc, comp_size, uncomp_size, local_offset, e.external_attributes });
    }

    u32 cd_offset = static_cast<u32>(out.size());
    for (auto const& r : records) {
        append_u32(out, 0x02014b50u);
        append_u16(out, (3u << 8) | 20u);
        append_u16(out, 20);
        append_u16(out, 0);
        append_u16(out, r.method);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u32(out, r.crc32);
        append_u32(out, r.comp_size);
        append_u32(out, r.uncomp_size);
        append_u16(out, static_cast<u16>(r.name.length()));
        append_u16(out, 0);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u16(out, 0);
        append_u32(out, r.external_attrs);
        append_u32(out, r.local_offset);
        MUST(out.try_append(r.name.bytes()));
    }
    u32 cd_size = static_cast<u32>(out.size()) - cd_offset;

    // EOCD
    append_u32(out, 0x06054b50u);
    append_u16(out, 0);
    append_u16(out, 0);
    append_u16(out, static_cast<u16>(records.size()));
    append_u16(out, static_cast<u16>(records.size()));
    append_u32(out, cd_size);
    append_u32(out, cd_offset);
    append_u16(out, 0);
    return out;
}

class ExtensionTestPageClient final : public Web::PageClient {
    GC_CELL(ExtensionTestPageClient, Web::PageClient);
    GC_DECLARE_ALLOCATOR(ExtensionTestPageClient);

public:
    virtual Web::PageId id() const override { return Web::PageId { 77 }; }
    virtual Web::Page& page() override { return *m_page; }
    virtual Web::Page const& page() const override { return *m_page; }
    virtual bool is_connection_open() const override { return true; }
    virtual Gfx::Palette palette() const override { VERIFY_NOT_REACHED(); }
    virtual Web::DevicePixelRect screen_rect() const override { return {}; }
    virtual double zoom_level() const override { return 1; }
    virtual double device_pixel_ratio() const override { return 1; }
    virtual double device_pixels_per_css_pixel() const override { return 1; }
    virtual Web::CSS::PreferredColorScheme preferred_color_scheme() const override { return Web::CSS::PreferredColorScheme::Auto; }
    virtual Web::CSS::PreferredContrast preferred_contrast() const override { return Web::CSS::PreferredContrast::Auto; }
    virtual Web::CSS::PreferredMotion preferred_motion() const override { return Web::CSS::PreferredMotion::NoPreference; }
    virtual size_t screen_count() const override { return 1; }
    virtual Queue<Web::QueuedInputEvent>& input_event_queue() override { VERIFY_NOT_REACHED(); }
    virtual void report_finished_handling_input_event(Web::PageId, u64, Web::EventResult) override { }
    virtual Web::HTML::CrossProcessId allocate_cross_process_id() override { return { 1, m_next_cross_process_id++ }; }
    virtual void request_frame() override { }
    virtual void request_file(Web::FileRequest) override { }
    virtual bool is_headless() const override { return true; }
    virtual void visit_edges(Visitor& visitor) override
    {
        Base::visit_edges(visitor);
        visitor.visit(m_page);
    }

    GC::Ptr<Web::Page> m_page;
    u64 m_next_cross_process_id { 1 };
};

GC_DEFINE_ALLOCATOR(ExtensionTestPageClient);

}

TEST_CASE(moz_extension_origin_and_secure_context)
{
    auto url_a1 = URL::Parser::basic_parse("moz-extension://11111111-2222-4333-8444-555555555555/popup.html"sv).release_value();
    auto url_a2 = URL::Parser::basic_parse("moz-extension://11111111-2222-4333-8444-555555555555/scripts/bg.js"sv).release_value();
    auto url_b = URL::Parser::basic_parse("moz-extension://aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee/popup.html"sv).release_value();

    EXPECT(!url_a1.origin().is_opaque());
    EXPECT_EQ(url_a1.origin().scheme(), "moz-extension"sv);
    EXPECT(url_a1.origin().is_same_origin(url_a2.origin()));
    EXPECT(!url_a1.origin().is_same_origin(url_b.origin()));

    EXPECT(Web::SecureContexts::is_url_potentially_trustworthy(url_a1) == Web::SecureContexts::Trustworthiness::PotentiallyTrustworthy);
    EXPECT(Web::SecureContexts::is_origin_potentially_trustworthy(url_a1.origin()) == Web::SecureContexts::Trustworthiness::PotentiallyTrustworthy);
}

TEST_CASE(match_patterns_and_globs)
{
    auto sub_pat = Web::Extensions::MatchPattern::parse("https://*.example.com/app/*"sv).release_value();
    auto https_app = URL::Parser::basic_parse("https://sub.example.com/app/index.html"sv).release_value();
    auto https_apex = URL::Parser::basic_parse("https://example.com/app/index.html"sv).release_value();
    auto https_other = URL::Parser::basic_parse("https://evil.org/app/index.html"sv).release_value();
    auto http_sub = URL::Parser::basic_parse("http://sub.example.com/app/index.html"sv).release_value();

    EXPECT(sub_pat.matches(https_app));
    EXPECT(sub_pat.matches(https_apex));
    EXPECT(!sub_pat.matches(https_other));
    EXPECT(!sub_pat.matches(http_sub));

    auto all_urls = Web::Extensions::MatchPattern::parse("<all_urls>"sv).release_value();
    EXPECT(all_urls.matches(https_app));
    EXPECT(all_urls.matches(http_sub));
    EXPECT(!all_urls.matches(URL::Parser::basic_parse("about:blank"sv).release_value()));

    auto file_url = URL::Parser::basic_parse("file:///tmp/report.html"sv).release_value();
    auto file = TRY_OR_FAIL(Web::Extensions::MatchPattern::parse("file:///*"sv));
    auto file_with_wildcard = TRY_OR_FAIL(Web::Extensions::MatchPattern::parse("file://*/*"sv));
    EXPECT(file.matches(file_url));
    EXPECT(file_with_wildcard.matches(file_url));
    EXPECT(!file.matches(https_app));
    EXPECT(!all_urls.matches(file_url));
    EXPECT(!all_urls.covers_origin(file));
    EXPECT(!Web::Extensions::MatchPattern::is_valid_host_permission_pattern("file:///*"sv));

    auto excl = Web::Extensions::MatchPattern::parse("https://sub.example.com/app/private/*"sv).release_value();
    auto priv_url = URL::Parser::basic_parse("https://sub.example.com/app/private/secret"sv).release_value();
    EXPECT(Web::Extensions::MatchPattern::matches_url_with_rules(https_app, { sub_pat }, { excl }, { "*index*"_string }, { "*secret*"_string }));
    EXPECT(!Web::Extensions::MatchPattern::matches_url_with_rules(priv_url, { sub_pat }, { excl }, {}, {}));
    EXPECT(!Web::Extensions::MatchPattern::matches_url_with_rules(https_app, { sub_pat }, {}, {}, { "*index*"_string }));
}

TEST_CASE(extension_manifest_mv2_mv3_and_csp)
{
    // MV2 Quicknote-style with sidebar_action and browser_action
    auto mv2 = Web::Extensions::ExtensionManifest::parse(R"({
        "manifest_version": 2,
        "name": "Quicknote Test",
        "version": "1.2.3",
        "browser_specific_settings": {
            "gecko": {
                "id": "quicknote@example.org",
                "strict_min_version": "109.0",
                "data_collection_permissions": {
                    "required": ["none"]
                }
            }
        },
        "permissions": ["storage", "tabs", "https://example.com/*"],
        "background": {
            "scripts": ["bg.js"],
            "persistent": false
        },
        "browser_action": {
            "default_popup": "/popup.html",
            "default_title": "Note"
        },
        "sidebar_action": {
            "default_panel": "sidebar/panel.html",
            "default_title": "Quicknote",
            "open_at_install": true
        },
        "web_accessible_resources": ["injected/*.css"]
    })"sv)
                   .release_value();

    EXPECT_EQ(mv2.manifest_version(), Web::Extensions::ManifestVersion::V2);
    EXPECT_EQ(mv2.gecko_id(), Optional<String> { "quicknote@example.org"_string });
    EXPECT(mv2.has_permission("storage"sv));
    EXPECT(mv2.has_host_permission_for(URL::Parser::basic_parse("https://example.com/foo"sv).release_value()));
    EXPECT(mv2.background().has_value());
    EXPECT(!mv2.background()->persistent);
    EXPECT(mv2.action().has_value());
    EXPECT_EQ(mv2.action()->default_popup, Optional<String> { "popup.html"_string });
    EXPECT(mv2.sidebar_action().has_value());
    EXPECT(mv2.sidebar_action()->open_at_install);
    EXPECT(mv2.is_web_accessible("injected/theme.css"sv, URL::Parser::basic_parse("https://other.test/"sv).release_value()));
    EXPECT(!mv2.is_web_accessible("bg.js"sv, URL::Parser::basic_parse("https://other.test/"sv).release_value()));
    EXPECT(!mv2.has_invalid_csp());

    // MV3 with invalid CSP ('unsafe-eval') falls back safely
    auto mv3 = Web::Extensions::ExtensionManifest::parse(R"({
        "manifest_version": 3,
        "name": "Beastify Test",
        "version": "2.0.0",
        "browser_specific_settings": { "gecko": { "id": "beastify@example.org" } },
        "permissions": ["activeTab", "scripting"],
        "host_permissions": ["https://*.allowed.test/*"],
        "action": { "default_popup": "popup/choose_beast.html" },
        "web_accessible_resources": [{
            "resources": ["beasts/*.jpg"],
            "matches": ["https://*.allowed.test/*"]
        }],
        "content_security_policy": {
            "extension_pages": "script-src 'self' 'unsafe-eval'; object-src 'self';"
        }
    })"sv)
                   .release_value();

    EXPECT_EQ(mv3.manifest_version(), Web::Extensions::ManifestVersion::V3);
    EXPECT(mv3.has_invalid_csp());
    EXPECT(!mv3.content_security_policy().bytes_as_string_view().contains("unsafe-eval"sv));
    EXPECT(mv3.is_web_accessible("beasts/frog.jpg"sv, URL::Parser::basic_parse("https://www.allowed.test/page"sv).release_value()));
    EXPECT(!mv3.is_web_accessible("beasts/frog.jpg"sv, URL::Parser::basic_parse("https://denied.test/page"sv).release_value()));

    EXPECT(Web::Extensions::ExtensionManifest::normalize_package_relative_path("../escape.js"sv).is_error());
    EXPECT(Web::Extensions::ExtensionManifest::normalize_package_relative_path("a\\b.js"sv).is_error());
}

TEST_CASE(action_theme_icons_require_local_paths_and_positive_sizes)
{
    auto parse = [](StringView icons) {
        return Web::Extensions::ExtensionManifest::parse(MUST(String::formatted(R"({{
            "manifest_version": 3, "name": "Colors", "version": "1",
            "action": {{ "theme_icons": {} }}
        }})",
            icons)));
    };
    EXPECT(parse(R"([{"size": 16, "light": "../escape.png", "dark": "dark.png"}])"sv).is_error());
    EXPECT(parse(R"([{"size": 0, "light": "light.png", "dark": "dark.png"}])"sv).is_error());
    EXPECT(parse(R"([{"size": 16, "dark": "dark.png"}])"sv).is_error());
    auto valid = TRY_OR_FAIL(parse(R"([{"size": 16, "light": "light.png", "dark": "dark.png"}])"sv));
    EXPECT_EQ(Web::Extensions::SupportMatrix::evaluate_manifest(valid).status, Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested);
}

TEST_CASE(navigation_url_filters_match_components_and_regular_expressions)
{
    auto filter = TRY_OR_FAIL(Web::Extensions::NavigationURLFilter::parse(TRY_OR_FAIL(JsonValue::from_string(R"({
        "url": [
            { "schemes": ["https"], "hostContains": ".example.", "ports": [443, [8000, 8080]],
              "pathPrefix": "/tools/", "queryEquals": "q=1", "urlMatches": "1$", "originAndPathMatches": "/tools/install$" },
            { "urlEquals": "about:blank" }
        ]
    })"sv))));
    auto matches = [&](StringView url) { return filter->matches(URL::Parser::basic_parse(url).release_value()); };
    EXPECT(matches("https://example.test/tools/install?q=1#done"sv));
    EXPECT(matches("https://www.example.test:8010/tools/install?q=1"sv));
    EXPECT(matches("about:blank#fragment"sv));
    EXPECT(!matches("https://otherexample.test/tools/install?q=1"sv));
    EXPECT(!matches("https://example.test:800/tools/install?q=1"sv));
    EXPECT(!matches("https://example.test/tools/INSTALL?q=1"sv));
    EXPECT(Web::Extensions::NavigationURLFilter::parse(TRY_OR_FAIL(JsonValue::from_string(R"({"url":[{"ports":[1.5]}]})"sv))).is_error());
    EXPECT(Web::Extensions::NavigationURLFilter::parse(TRY_OR_FAIL(JsonValue::from_string(R"({"url":[{"urlMatches":"["}]})"sv))).is_error());
}

TEST_CASE(support_matrix_classification)
{
    auto compatible = Web::Extensions::ExtensionManifest::parse(R"({
        "manifest_version": 3,
        "name": "Compat",
        "version": "1.0",
        "permissions": ["activeTab", "scripting", "storage"]
    })"sv)
                          .release_value();
    auto eval_compat = Web::Extensions::SupportMatrix::evaluate_manifest(compatible);
    EXPECT_EQ(eval_compat.status, Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested);

    auto tested_list = Web::Extensions::SupportMatrix::tested_extension_versions();
    EXPECT(!tested_list.is_empty());

    constexpr Web::Extensions::TestedExtensionVersion sample_tested {
        .addon_id = "tested-fixture@ladyflock.example"sv,
        .version = "1.2.3"sv,
        .sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"sv,
    };
    Array extra_tested { sample_tested };
    auto compat_tested = Web::Extensions::ExtensionManifest::parse(MUST(String::formatted(R"({{
        "manifest_version": 2,
        "name": "Tested",
        "version": "{}",
        "browser_specific_settings": {{ "gecko": {{ "id": "{}" }} }},
        "permissions": ["storage"]
    }})",
                                                                       sample_tested.version, sample_tested.addon_id)))
                             .release_value();
    auto eval_tested = Web::Extensions::SupportMatrix::evaluate_manifest(compat_tested, sample_tested.sha256, extra_tested.span());
    EXPECT_EQ(eval_tested.status, Web::Extensions::CompatibilityStatus::TestedInLadyflock);
    auto eval_untested_hash = Web::Extensions::SupportMatrix::evaluate_manifest(compat_tested, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"sv, extra_tested.span());
    EXPECT_EQ(eval_untested_hash.status, Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested);

    auto unsupported = Web::Extensions::ExtensionManifest::parse(R"({
        "manifest_version": 3,
        "name": "Bad",
        "version": "1.0",
        "permissions": ["bookmarks"],
        "background": { "service_worker": "sw.js" }
    })"sv)
                           .release_value();
    auto eval_bad = Web::Extensions::SupportMatrix::evaluate_manifest(unsupported);
    EXPECT_EQ(eval_bad.status, Web::Extensions::CompatibilityStatus::Incompatible);
    EXPECT(eval_bad.unsupported_reasons.size() >= 2);

    // A module background and a content script in the page's own world both run, as in Firefox.
    auto execution_options = TRY_OR_FAIL(Web::Extensions::ExtensionManifest::parse(R"({
        "manifest_version": 3,
        "name": "Execution settings",
        "version": "1",
        "background": { "scripts": ["background.js"], "type": "module" },
        "content_scripts": [{
            "matches": ["https://example.com/*"],
            "js": ["content.js"],
            "world": "MAIN",
            "match_about_blank": true
        }]
    })"sv));
    auto eval_options = Web::Extensions::SupportMatrix::evaluate_manifest(execution_options);
    EXPECT_EQ(eval_options.status, Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested);
    EXPECT(eval_options.unsupported_reasons.is_empty());
    EXPECT(execution_options.background()->is_module);
    EXPECT(execution_options.content_scripts().first().match_about_blank);
    EXPECT(execution_options.content_scripts().first().main_world);

    auto unbound_commands = TRY_OR_FAIL(Web::Extensions::ExtensionManifest::parse(R"({
        "manifest_version": 3, "name": "Commands", "version": "1",
        "commands": { "unbound": { "description": "Unbound command" } }
    })"sv));
    EXPECT_EQ(Web::Extensions::SupportMatrix::evaluate_manifest(unbound_commands).status, Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested);
    auto bound_commands = TRY_OR_FAIL(Web::Extensions::ExtensionManifest::parse(R"({
        "manifest_version": 3, "name": "Commands", "version": "1",
        "commands": { "bound": { "suggested_key": { "default": "Ctrl+Shift+Y" } } }
    })"sv));
    // A suggested shortcut is accepted. Ladyflock binds no shortcut yet, so commands.getAll reports none, as Firefox
    // does for a command without one.
    EXPECT_EQ(Web::Extensions::SupportMatrix::evaluate_manifest(bound_commands).status, Web::Extensions::CompatibilityStatus::ManifestCompatibleUntested);
}

TEST_CASE(zip_reader_hardening)
{
    // 1. Valid deflate entry
    auto hello = MUST(ByteBuffer::copy("hello world from extension"sv.bytes()));
    auto good_zip = build_test_zip({ ZipFixtureEntry { "manifest.json", hello, true, 0, false, 0, false, 0 } });
    auto parsed = Web::Extensions::ZipReader::parse(good_zip.bytes()).release_value();
    auto extracted = parsed.extract_entry_by_name("manifest.json"sv).release_value();
    EXPECT_EQ(extracted.bytes(), hello.bytes());

    // 2. Path traversal rejected
    auto traversal_zip = build_test_zip({ ZipFixtureEntry { "../evil.js", hello } });
    EXPECT(Web::Extensions::ZipReader::parse(traversal_zip.bytes()).is_error());

    // 3. Symlink entry rejected (0120000 << 16)
    auto symlink_zip = build_test_zip({ ZipFixtureEntry { "link.js", hello, false, 0, false, 0, false, 0120000u << 16 } });
    EXPECT(Web::Extensions::ZipReader::parse(symlink_zip.bytes()).is_error());

    // 4. Duplicate entry rejected
    auto dup_zip = build_test_zip({ ZipFixtureEntry { "a.js", hello }, ZipFixtureEntry { "a.js", hello } });
    EXPECT(Web::Extensions::ZipReader::parse(dup_zip.bytes()).is_error());

    // 5. CRC mismatch rejected
    auto bad_crc_zip = build_test_zip({ ZipFixtureEntry { "a.js", hello, false, 0xdeadbeefu, true } });
    auto parsed_crc = Web::Extensions::ZipReader::parse(bad_crc_zip.bytes()).release_value();
    EXPECT(parsed_crc.extract_entry_by_name("a.js"sv).is_error());

    // 6. Zip bomb ratio rejected
    auto zero_buf = MUST(ByteBuffer::create_zeroed(128 * 1024));
    auto compressed = deflate_raw_bytes(zero_buf.bytes());
    u32 declared_uncompressed = max(static_cast<u32>(compressed.size() * 500u), static_cast<u32>(zero_buf.size()));
    auto bomb_zip = build_test_zip({ ZipFixtureEntry { "bomb.bin", zero_buf, true, 0, false, declared_uncompressed, true } });
    EXPECT(Web::Extensions::ZipReader::parse(bomb_zip.bytes()).is_error());
}

static void install_test_font_plugin()
{
    static Web::Platform::FontPlugin font_plugin { false };
    static bool installed = false;
    if (installed)
        return;
    Web::Platform::FontPlugin::install(font_plugin);
    installed = true;
}

TEST_CASE(extension_content_script_host_isolated_world_and_css)
{
    install_test_font_plugin();
    auto principal_realm = Web::Bindings::create_a_principal_javascript_realm();
    VERIFY(principal_realm.ptr());
    auto& vm = Web::Bindings::main_thread_vm();

    auto client = vm.heap().allocate<ExtensionTestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();

    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto document = GC::Ref { *traversable->active_document() };

    auto& window_realm = document->window()->principal_realm();
    auto page_script = JS::Script::parse("window.pageVar = 'from-page'; window.extLeak = undefined;"_utf16, window_realm).release_value();
    (void)vm.run(page_script);

    Web::Extensions::AttachedContentScript attached;
    attached.extension_id = "ext@test"_string;
    attached.extension_uuid = "11111111-2222-4333-8444-555555555555"_string;
    attached.grant_generation = 1;
    attached.run_at = Web::Extensions::RunAt::DocumentStart;
    attached.css_files.append({ "user.css"_string, "body { background-color: rgb(1, 2, 3); }"_string });
    attached.js_files.append({
        "content.js"_string,
        "window.extLeak = 'hidden'; document.documentElement.setAttribute('data-ext-ran', typeof window.pageVar === 'undefined' ? 'isolated' : 'leaked');"_string,
    });

    auto& host = document->ensure_extension_content_script_host();
    host.attach_scripts({ attached });
    host.run_scripts_for(Web::Extensions::RunAt::DocumentStart);

    // Content script ran in an isolated world (could mutate shared DOM, could not read page JS expando, did not pollute page window).
    EXPECT_EQ(document->document_element()->get_attribute("data-ext-ran"_utf16_fly_string), Optional<Utf16String> { "isolated"_utf16 });
    auto check_leak = JS::Script::parse("window.extLeak === undefined;"_utf16, window_realm).release_value();
    auto check_val = vm.run(check_leak).release_value();
    EXPECT(check_val.as_bool());

    // Extension CSS accumulated into document user stylesheet source.
    EXPECT(document->extension_style_sheet_source().has_value());
    auto css_after_attach = document->extension_style_sheet_source()->to_utf8();
    EXPECT(css_after_attach.bytes_as_string_view().contains("rgb(1, 2, 3)"sv));

    // Revoking clears extension styles and detaches realm.
    host.revoke_extension("ext@test"sv, 2);
    if (document->extension_style_sheet_source().has_value()) {
        auto css_after_revoke = document->extension_style_sheet_source()->to_utf8();
        EXPECT(!css_after_revoke.bytes_as_string_view().contains("rgb(1, 2, 3)"sv));
    }
}

TEST_CASE(extension_content_script_window_matches_pending_document)
{
    install_test_font_plugin();
    auto principal_realm = Web::Bindings::create_a_principal_javascript_realm();
    VERIFY(principal_realm.ptr());
    auto& vm = Web::Bindings::main_thread_vm();

    auto client = vm.heap().allocate<ExtensionTestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto active_document = GC::Ref { *traversable->active_document() };
    auto browsing_context = GC::Ref { *active_document->browsing_context() };

    auto next_url = URL::Parser::basic_parse("https://next.example/"sv).release_value();
    GC::Ptr<Web::HTML::Window> next_window;
    auto execution_context = Web::HTML::create_window_realm(next_window, *browsing_context, Web::HTML::CrossOriginIsolationMode::None);
    Web::HTML::WindowEnvironmentSettingsObject::setup(*page, next_url, move(execution_context), nullptr, next_url, next_url.origin(), {}, Web::HTML::CrossOriginIsolationMode::None);
    auto next_document = Web::HTML::HTMLDocument::create(*page, next_window.as_nonnull(), next_url);
    next_document->set_window(next_window.as_nonnull());
    next_document->set_origin(next_url.origin());
    next_document->set_browsing_context(browsing_context);
    next_window->set_associated_document(*next_document);

    auto extension = Web::Bindings::ExtensionRealmContext::create_for_content_script(*next_document,
        "ext@test"_string, "11111111-2222-4333-8444-555555555555"_string, 1);
    ScopeGuard revoke_extension = [&] { extension->revoke_and_detach(); };
    auto before_activation = extension->evaluate_script(R"(
        self.filterState = 'pending';
        self === globalThis && self === window && self === document.defaultView
            && self.document === document && self.location === document.location
            && document.location.origin === 'https://next.example'
            && typeof self.browser === 'object';
    )"sv,
        "start.js"sv, true);
    EXPECT_EQ(before_activation, Optional<String> { R"({"result":true})"_string });

    browsing_context->set_active_window(next_window.as_nonnull());
    auto after_activation = extension->evaluate_script("self.filterState === 'pending' && self === window"sv, "idle.js"sv, true);
    EXPECT_EQ(after_activation, Optional<String> { R"({"result":true})"_string });

    // Retarget the browsing context's ordinary proxy and check that the extension keeps its own document.
    browsing_context->set_active_window(GC::Ref { *active_document->window() });
    auto after_retarget = extension->evaluate_script("self.filterState === 'pending' && document.location.hostname === 'next.example'"sv, "retarget.js"sv, true);
    EXPECT_EQ(after_retarget, Optional<String> { R"({"result":true})"_string });
}

TEST_CASE(extension_public_suffix_api_requires_permission)
{
    install_test_font_plugin();
    auto principal_realm = Web::Bindings::create_a_principal_javascript_realm();
    VERIFY(principal_realm.ptr());
    auto& vm = Web::Bindings::main_thread_vm();
    auto client = vm.heap().allocate<ExtensionTestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);
    auto& document = *traversable->active_document();

    auto without_permission = Web::Bindings::ExtensionRealmContext::create_for_extension_page(document,
        "psl@test"_string, "11111111-2222-4333-8444-555555555555"_string, R"({"optional_permissions":["publicSuffix"]})"_string);
    auto absent = without_permission->evaluate_script("browser.publicSuffix === undefined"sv, "public-suffix.js"sv, true);
    EXPECT_EQ(absent, Optional<String> { R"({"result":true})"_string });

    auto with_permission = Web::Bindings::ExtensionRealmContext::create_for_extension_page(document,
        "psl@test"_string, "11111111-2222-4333-8444-555555555555"_string, R"({"permissions":["publicSuffix"]})"_string);
    auto present = with_permission->evaluate_script(R"(
        (() => {
            const api = browser.publicSuffix;
            let invalidRejected = false;
            try { api.getKnownSuffix('https://example.com'); } catch (error) { invalidRejected = error instanceof TypeError; }
            return invalidRejected
                && api.isKnownSuffix('co.uk') && !api.isKnownSuffix('example.co.uk')
                && api.getKnownSuffix('a.b.co.uk') === 'co.uk'
                && api.getDomain('a.b.co.uk') === 'b.co.uk'
                && api.getDomain('a.unknown-test') === null
                && api.getDomain('a.unknown-test', { allowUnknownSuffix: true }) === 'a.unknown-test'
                && api.getDomain('co.uk', { allowPlainSuffix: true }) === 'co.uk'
                && api.getDomain('[::1]') === null
                && api.getDomain('[::1]', { allowIPAddress: true }) === '[::1]'
                && api.getDomain('www.bücher.de') === 'xn--bcher-kva.de'
                && api.getDomain('www.bücher.de', { encoding: 'display' }) === 'bücher.de';
        })()
    )"sv,
        "public-suffix.js"sv, true);
    EXPECT_EQ(present, Optional<String> { R"({"result":true})"_string });
}

TEST_CASE(extension_page_localized_messages)
{
    install_test_font_plugin();
    auto principal_realm = Web::Bindings::create_a_principal_javascript_realm();
    VERIFY(principal_realm.ptr());
    auto& vm = Web::Bindings::main_thread_vm();
    auto client = vm.heap().allocate<ExtensionTestPageClient>();
    auto page = Web::Page::create(client);
    client->m_page = page.ptr();
    auto traversable = Web::HTML::LocalTraversableNavigable::create_a_new_top_level_traversable(page, nullptr, {});
    page->set_top_level_traversable(traversable);

    auto context = Web::Bindings::ExtensionRealmContext::create_for_extension_page(*traversable->active_document(),
        "messages@test"_string, "11111111-2222-4333-8444-555555555555"_string, "{}"_string, R"({
            "@@ui_locale": {"message":"fr_CA"},
            "welcome": {"message":"$GUEST$: $$ $2", "placeholders":{"guest":{"content":"$1"}}}
        })"_string);
    auto result = context->evaluate_script(R"(
        browser.i18n.getMessage('Welcome', ['Josée', 'bonjour']) === 'Josée: $ bonjour'
            && chrome.i18n.getUILanguage() === 'fr-CA'
            && browser.i18n.getMessage('missing') === ''
    )"sv,
        "messages.js"sv, true);
    EXPECT_EQ(result, Optional<String> { R"({"result":true})"_string });
}

// Firefox restricts only the sources of an extension page's scripts, workers and plugins.
TEST_CASE(extension_csp_restricts_script_sources_only)
{
    using Web::Extensions::ExtensionManifest;
    using Web::Extensions::ManifestVersion;
    struct Case {
        StringView policy;
        bool valid_mv2;
        bool valid_mv3;
    };
    static constexpr Array cases {
        // 1Password's policy, trimmed: local and remote origins in connect-src and form-action are the extension's own business.
        Case { "default-src 'none'; form-action https://localhost:3030; frame-ancestors http: https: 'self'; connect-src http://127.0.0.1:12519 wss://b5n.1password.com https:; script-src 'self' 'wasm-unsafe-eval'"sv, true, true },
        Case { "script-src 'self'; object-src 'self'"sv, true, true },
        Case { "script-src 'self' 'sha256-abc='; img-src *"sv, true, true },
        Case { "default-src 'self'"sv, true, true },
        Case { "script-src 'self' 'unsafe-eval'"sv, true, false },
        Case { "script-src 'self' https://cdn.example.com"sv, true, false },
        Case { "script-src 'self' http://localhost:8080"sv, true, false },
        Case { "script-src 'self' 'unsafe-inline'"sv, false, false },
        Case { "script-src 'self' https:"sv, false, false },
        Case { "script-src 'self' https://*.example.com"sv, false, false },
        Case { "script-src 'self' http://example.com"sv, false, false },
        Case { "script-src *"sv, false, false },
        Case { "script-src 'self' data:"sv, false, false },
        // script-src-elem and script-src-attr take precedence over script-src, so they're held to the same sources.
        Case { "script-src 'self'; script-src-elem https:"sv, false, false },
        Case { "script-src 'self'; script-src-attr 'unsafe-inline'"sv, false, false },
        // Workers fall back to child-src before script-src.
        Case { "script-src 'self'; child-src https:"sv, false, false },
        Case { "script-src 'self'; worker-src blob:"sv, true, false },
        // Plugins are gone in Manifest V3, so only Manifest V2 restricts object-src.
        Case { "script-src 'self'; object-src https:"sv, false, true },
        // Without script-src or default-src, scripts are unrestricted.
        Case { "img-src 'self'"sv, false, false },
        Case { ""sv, false, false },
    };
    for (auto const& test : cases) {
        auto mv2 = ExtensionManifest::is_valid_extension_csp(test.policy, ManifestVersion::V2);
        auto mv3 = ExtensionManifest::is_valid_extension_csp(test.policy, ManifestVersion::V3);
        if (mv2 != test.valid_mv2 || mv3 != test.valid_mv3)
            warnln("Policy \"{}\": MV2 {} (expected {}), MV3 {} (expected {})", test.policy, mv2, test.valid_mv2, mv3, test.valid_mv3);
        EXPECT_EQ(mv2, test.valid_mv2);
        EXPECT_EQ(mv3, test.valid_mv3);
    }

    // An invalid policy refuses the install, as a manifest error does in Firefox.
    auto unsafe = MUST(ExtensionManifest::parse(R"({"manifest_version": 2, "name": "A", "version": "1",
        "content_security_policy": "script-src 'self' 'unsafe-inline'"})"sv));
    EXPECT(unsafe.has_invalid_csp());
    EXPECT_EQ(Web::Extensions::SupportMatrix::evaluate_manifest(unsafe).status, Web::Extensions::CompatibilityStatus::Incompatible);
}
