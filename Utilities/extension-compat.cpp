/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/LexicalPath.h>
#include <LibCore/ArgsParser.h>
#include <LibCore/File.h>
#include <LibMain/Main.h>
#include <LibWebCommon/Extensions/ExtensionManifest.h>
#include <LibWebCommon/Extensions/SupportMatrix.h>
#include <LibWebCommon/Extensions/ZipReader.h>
#include <LibWebView/Extensions/DeclarativeRequestRules.h>

// Reports, for each XPI, whether Ladyflock's install check accepts its manifest and its declarative rulesets, and why
// not. It reads the package only: it does not check the signature, and it runs none of the extension's code.
static JsonObject evaluate_package(StringView path)
{
    JsonObject result;
    auto package_name = LexicalPath::basename(path);
    result.set("package"sv, MUST(String::from_byte_string(package_name)));
    auto fail = [&](StringView stage, auto const& error) {
        result.set("status"sv, "unreadable"sv);
        result.set("error"sv, MUST(String::formatted("{}: {}", stage, error)));
        return result;
    };

    auto file = Core::File::open(path, Core::File::OpenMode::Read);
    if (file.is_error())
        return fail("open"sv, file.error());
    auto bytes = file.value()->read_until_eof();
    if (bytes.is_error())
        return fail("read"sv, bytes.error());
    auto zip = Web::Extensions::ZipReader::parse(bytes.value());
    if (zip.is_error())
        return fail("archive"sv, zip.error());
    auto manifest_bytes = zip.value().extract_entry_by_name("manifest.json"sv);
    if (manifest_bytes.is_error())
        return fail("manifest.json"sv, manifest_bytes.error());
    auto manifest_text = String::from_utf8(StringView { manifest_bytes.value().bytes() });
    if (manifest_text.is_error())
        return fail("manifest.json"sv, manifest_text.error());
    auto manifest = Web::Extensions::ExtensionManifest::parse(manifest_text.value());
    if (manifest.is_error())
        return fail("manifest"sv, manifest.error());

    auto evaluation = Web::Extensions::SupportMatrix::evaluate_manifest(manifest.value());
    // The installer also loads the packaged rulesets, and refuses the extension if one doesn't parse.
    auto rulesets = WebView::Extensions::load_declarative_rulesets(manifest.value(), [&](StringView path) -> ErrorOr<String> {
        auto bytes = TRY(zip.value().extract_entry_by_name(path));
        return String::from_utf8(StringView { bytes.bytes() });
    });
    if (rulesets.is_error()) {
        evaluation.status = Web::Extensions::CompatibilityStatus::Incompatible;
        evaluation.unsupported_reasons.append(MUST(String::formatted("Declarative rules: {}", rulesets.error())));
    }
    result.set("status"sv, MUST(String::from_utf8(Web::Extensions::to_string(evaluation.status))));
    result.set("id"sv, manifest.value().gecko_id().value_or({}));
    result.set("version"sv, manifest.value().version());
    JsonArray reasons;
    for (auto const& reason : evaluation.unsupported_reasons)
        reasons.must_append(reason);
    result.set("reasons"sv, move(reasons));
    JsonArray warnings;
    for (auto const& warning : evaluation.warnings)
        warnings.must_append(warning);
    result.set("warnings"sv, move(warnings));
    return result;
}

ErrorOr<int> ladybird_main(Main::Arguments arguments)
{
    Vector<StringView> packages;
    Core::ArgsParser args_parser;
    args_parser.set_general_help("Print, as one JSON object per line, whether Ladyflock would install each XPI.");
    args_parser.add_positional_argument(packages, "XPI files to check", "xpi");
    args_parser.parse(arguments);

    for (auto path : packages)
        outln("{}", evaluate_package(path).serialized());
    return 0;
}
