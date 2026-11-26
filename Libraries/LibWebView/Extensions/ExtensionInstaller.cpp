/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/HashTable.h>
#include <AK/LexicalPath.h>
#include <AK/Random.h>
#include <LibCore/DirIterator.h>
#include <LibCore/Directory.h>
#include <LibCore/File.h>
#include <LibCore/System.h>
#include <LibFileSystem/FileSystem.h>
#include <LibWebCommon/Extensions/ZipReader.h>
#include <LibWebView/Extensions/DeclarativeRequestRules.h>
#include <LibWebView/Extensions/ExtensionInstaller.h>
#include <LibWebView/HelperProcess.h>

namespace WebView::Extensions {

static ErrorOr<void> ensure_parent_directory(ByteString const& path)
{
    auto parent = LexicalPath::dirname(path);
    if (!parent.is_empty() && parent != "."sv)
        TRY(Core::Directory::create(parent, Core::Directory::CreateDirectories::Yes));
    return {};
}

static Vector<String> collect_host_permissions(Web::Extensions::ExtensionManifest const& manifest)
{
    Vector<String> permissions;
    for (auto const& pattern : manifest.host_permissions())
        permissions.append(pattern.raw());
    for (auto const& rule : manifest.content_scripts()) {
        for (auto const& pattern : rule.matches) {
            if (!permissions.contains_slow(pattern.raw()))
                permissions.append(pattern.raw());
        }
    }
    return permissions;
}

static Vector<String> compute_newly_requested_permissions(
    Web::Extensions::ExtensionManifest const& next_manifest,
    Optional<Web::Extensions::ExtensionManifest> const& current_manifest)
{
    Vector<String> added;
    if (!current_manifest.has_value()) {
        added.extend(next_manifest.permissions());
        added.extend(collect_host_permissions(next_manifest));
        added.extend(next_manifest.data_collection_permissions().required);
        return added;
    }

    for (auto const& perm : next_manifest.permissions()) {
        if (!current_manifest->has_permission(perm))
            added.append(perm);
    }
    auto existing_hosts = collect_host_permissions(*current_manifest);
    for (auto const& host : collect_host_permissions(next_manifest)) {
        if (!existing_hosts.contains_slow(host))
            added.append(host);
    }
    for (auto const& dc : next_manifest.data_collection_permissions().required) {
        if (!current_manifest->data_collection_permissions().required.contains_slow(dc))
            added.append(MUST(String::formatted("data_collection:{}", dc)));
    }
    return added;
}

ExtensionInstaller::ExtensionInstaller(ByteString extensions_root, RevocationList const& revocation_list)
    : m_extensions_root(move(extensions_root))
    , m_revocation_list(revocation_list)
{
}

ExtensionInstaller::~ExtensionInstaller() = default;

ByteString ExtensionInstaller::staging_root() const
{
    return LexicalPath::join(m_extensions_root, "staging"sv).string();
}

ByteString ExtensionInstaller::installed_root() const
{
    return LexicalPath::join(m_extensions_root, "installed"sv).string();
}

void ExtensionInstaller::cleanup_stale_staging()
{
    auto root = staging_root();
    if (FileSystem::exists(root))
        (void)FileSystem::remove(root, FileSystem::RecursionMode::Allowed);
    (void)Core::Directory::create(root, Core::Directory::CreateDirectories::Yes);
    (void)Core::Directory::create(installed_root(), Core::Directory::CreateDirectories::Yes);
}

ErrorOr<NonnullRefPtr<ExtensionVerifierClient>> ExtensionInstaller::ensure_verifier_client()
{
    if (m_verifier_client && m_verifier_client->is_open())
        return NonnullRefPtr { *m_verifier_client };
    m_verifier_client = TRY(launch_extension_verifier_process());
    return NonnullRefPtr { *m_verifier_client };
}

static String generate_token()
{
    return generate_random_uuid();
}

static ErrorOr<ByteBuffer> read_extension_file(ByteString const& path, size_t max_size)
{
    auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read | Core::File::OpenMode::Nonblocking | Core::File::OpenMode::NoFollow));
    auto info = TRY(file->stat());
    if (!S_ISREG(info.st_mode) || info.st_size < 0 || static_cast<u64>(info.st_size) > max_size)
        return Error::from_string_literal("Extension file is not a regular file or exceeds the size limit");

    ByteBuffer contents;
    Array<u8, 65536> buffer;
    for (;;) {
        auto bytes = TRY(file->read_some(buffer.span()));
        if (bytes.is_empty())
            break;
        if (bytes.size() > max_size - contents.size())
            return Error::from_string_literal("Extension file exceeds the size limit");
        TRY(contents.try_append(bytes));
    }
    return contents;
}

// Firefox's toolkit version format: dot-separated parts, each a number, a string, a number and a string. A missing
// number is 0 and a missing string sorts after any other, so 1.0pre1 < 1.0. See Mozilla's nsVersionComparator.
int compare_extension_versions(StringView left, StringView right)
{
    struct Part {
        i64 number_a { 0 };
        Optional<StringView> string_b;
        i64 number_c { 0 };
        Optional<StringView> string_d;
    };
    // Like strtol: an optional sign and digits, or nothing at all. Large numbers saturate as they do in Firefox.
    auto parse_number = [](StringView& text) -> i64 {
        size_t length = 0;
        bool negative = false;
        if (!text.is_empty() && (text[0] == '+' || text[0] == '-')) {
            negative = text[0] == '-';
            ++length;
        }
        size_t digits_start = length;
        i64 value = 0;
        while (length < text.length() && is_ascii_digit(text[length])) {
            value = min<i64>(value * 10 + parse_ascii_digit(text[length]), NumericLimits<i32>::max());
            ++length;
        }
        if (length == digits_start)
            return 0;
        text = text.substring_view(length);
        return negative ? -value : value;
    };
    auto parse_part = [&](StringView text) {
        Part part;
        if (text == "*"sv) {
            part.number_a = NumericLimits<i32>::max();
            return part;
        }
        part.number_a = parse_number(text);
        if (text.is_empty())
            return part;
        // "+" means the next prerelease: 1.0+ is 1.1pre.
        if (text[0] == '+') {
            ++part.number_a;
            part.string_b = "pre"sv;
            return part;
        }
        auto number_start = text.find_any_of("0123456789+-"sv);
        if (!number_start.has_value()) {
            part.string_b = text;
            return part;
        }
        part.string_b = text.substring_view(0, *number_start);
        text = text.substring_view(*number_start);
        part.number_c = parse_number(text);
        if (!text.is_empty())
            part.string_d = text;
        return part;
    };
    auto compare_strings = [](Optional<StringView> a, Optional<StringView> b) {
        if (!a.has_value() || !b.has_value())
            return a.has_value() == b.has_value() ? 0 : (a.has_value() ? -1 : 1);
        return *a < *b ? -1 : (*a == *b ? 0 : 1);
    };
    auto compare_numbers = [](i64 a, i64 b) { return a < b ? -1 : (a == b ? 0 : 1); };

    auto left_parts = left.split_view('.', SplitBehavior::KeepEmpty);
    auto right_parts = right.split_view('.', SplitBehavior::KeepEmpty);
    for (size_t i = 0; i < max(left_parts.size(), right_parts.size()); ++i) {
        auto a = parse_part(i < left_parts.size() ? left_parts[i] : StringView {});
        auto b = parse_part(i < right_parts.size() ? right_parts[i] : StringView {});
        if (auto result = compare_numbers(a.number_a, b.number_a); result != 0)
            return result;
        if (auto result = compare_strings(a.string_b, b.string_b); result != 0)
            return result;
        if (auto result = compare_numbers(a.number_c, b.number_c); result != 0)
            return result;
        if (auto result = compare_strings(a.string_d, b.string_d); result != 0)
            return result;
    }
    return 0;
}

ErrorOr<StagedInstall> ExtensionInstaller::stage_xpi(
    ByteString const& xpi_path,
    Optional<String> const& expected_addon_id,
    Optional<Web::Extensions::ExtensionManifest> const& current_manifest_for_update,
    Optional<String> const& previous_sha256_hex,
    InstallDiagnostic* diagnostic)
{
    return stage_package(TRY(read_extension_file(xpi_path, max_package_size)), expected_addon_id, {}, {}, current_manifest_for_update, previous_sha256_hex, diagnostic);
}

ErrorOr<ExtensionInstaller::VerifiedPackage> ExtensionInstaller::verify_package(ByteBuffer xpi_bytes, Optional<String> const& expected_addon_id, InstallDiagnostic* diagnostic)
{
    if (m_verification_disabled)
        return Error::from_string_literal("Signed extension verification is unavailable because revocation data could not be verified");
    if (xpi_bytes.is_empty() || xpi_bytes.size() > max_package_size)
        return Error::from_string_literal("Extension package is empty or exceeds 64 MiB");
    auto verifier = TRY(ensure_verifier_client());
    auto response = send_sync_to_helper<Messages::ExtensionVerifierServer::VerifyXpi>(
        *verifier, move(xpi_bytes), expected_addon_id, m_trusted_root_pems,
        m_revocation_list.revoked_addon_ids(), m_revocation_list.revoked_sha256_hexes());
    if (!response)
        return Error::from_string_literal("ExtensionVerifier helper process failed or timed out");
    auto result = response->take_result();
    if (!result.ok || !result.verified_signature) {
        if (diagnostic)
            diagnostic->verification_error = move(result.error);
        return Error::from_string_literal("Extension package could not be verified");
    }
    if (m_revocation_list.is_revoked(result.addon_id, result.version, result.sha256_hex))
        return Error::from_string_literal("Extension is revoked");
    if (result.compatibility_status == Web::Extensions::CompatibilityStatus::Incompatible) {
        if (diagnostic)
            diagnostic->compatibility_reasons = move(result.compatibility_reasons);
        return Error::from_string_literal("Extension is incompatible with Ladyflock");
    }
    auto names = response->take_file_names();
    auto contents = response->take_file_contents();
    if (names.size() != contents.size() || names.is_empty() || names.size() > Web::Extensions::ZipReader::max_entries)
        return Error::from_string_literal("Invalid verified extension payload");
    for (size_t i = 0, total_size = 0; i < names.size(); ++i) {
        auto name = TRY(Web::Extensions::ZipReader::validate_entry_name(names[i]));
        if (name.ends_with('/') || name.contains(':') || contents[i].size() > Web::Extensions::ZipReader::max_total_uncompressed_bytes - total_size)
            return Error::from_string_literal("Invalid verified extension file");
        total_size += contents[i].size();
    }
    return VerifiedPackage { move(result), move(names), move(contents) };
}

ErrorOr<StagedInstall> ExtensionInstaller::stage_package(
    ByteBuffer xpi_bytes,
    Optional<String> const& expected_addon_id,
    Optional<String> const& expected_version,
    Optional<String> const& expected_sha256_hex,
    Optional<Web::Extensions::ExtensionManifest> const& current_manifest_for_update,
    Optional<String> const& previous_sha256_hex,
    InstallDiagnostic* diagnostic)
{
    if (xpi_bytes.is_empty() || xpi_bytes.size() > max_package_size)
        return Error::from_string_literal("Extension package is empty or exceeds 64 MiB");

    auto token = generate_token();
    auto stage_dir = LexicalPath::join(staging_root(), token.bytes_as_string_view()).string();
    TRY(Core::Directory::create(stage_dir, Core::Directory::CreateDirectories::Yes));
    bool keep_stage_dir = false;
    ScopeGuard cleanup_on_error = [&] {
        if (!keep_stage_dir)
            (void)FileSystem::remove(stage_dir, FileSystem::RecursionMode::Allowed);
    };

    // Retain the exact snapshot handed to the verifier. The verifier returns the
    // validated payload, so the browser process never parses the input archive.
    auto archive_path = LexicalPath::join(stage_dir, "package.xpi"sv).string();
    auto archive = TRY(Core::File::open(archive_path, Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew, 0600));
    TRY(archive->write_until_depleted(xpi_bytes.bytes()));
    archive->close();

    auto verified = TRY(verify_package(move(xpi_bytes), expected_addon_id, diagnostic));
    auto const& result = verified.metadata;
    if (expected_version.has_value() && result.version != *expected_version)
        return Error::from_string_literal("Package version does not match the Mozilla Add-ons listing");
    if (expected_sha256_hex.has_value() && !result.sha256_hex.equals_ignoring_ascii_case(*expected_sha256_hex))
        return Error::from_string_literal("Package checksum does not match the Mozilla Add-ons listing");

    auto manifest = TRY(Web::Extensions::ExtensionManifest::parse(result.manifest_json));
    auto rulesets = load_declarative_rulesets(manifest, [&](StringView path) -> ErrorOr<String> {
        for (size_t i = 0; i < verified.file_names.size(); ++i) {
            if (verified.file_names[i] == path) {
                auto const& contents = verified.file_contents[i];
                if (contents.size() > 16 * MiB)
                    return Error::from_string_literal("Declarative ruleset exceeds its size limit");
                return String::from_utf8(StringView { contents.bytes() });
            }
        }
        return Error::from_string_literal("Declarative ruleset is missing from the signed package");
    });
    if (rulesets.is_error()) {
        auto error = rulesets.release_error();
        if (diagnostic)
            diagnostic->compatibility_reasons.append(TRY(String::formatted("{}", error)));
        return error;
    }
    if (current_manifest_for_update.has_value() && compare_extension_versions(result.version, current_manifest_for_update->version()) <= 0)
        return Error::from_string_literal("Update needs a newer extension version");

    auto payload_dir = LexicalPath::join(stage_dir, "payload"sv).string();
    TRY(Core::Directory::create(payload_dir, Core::Directory::CreateDirectories::Yes));
    for (size_t i = 0; i < verified.file_names.size(); ++i) {
        auto target_path = LexicalPath::join(payload_dir, verified.file_names[i]).string();
        TRY(ensure_parent_directory(target_path));
        auto out_file = TRY(Core::File::open(target_path, Core::File::OpenMode::Write | Core::File::OpenMode::MustBeNew, 0600));
        TRY(out_file->write_until_depleted(verified.file_contents[i].bytes()));
    }

    Vector<String> opt_host_perms;
    for (auto const& hp : manifest.optional_host_permissions())
        opt_host_perms.append(hp.raw());

    auto newly_requested = compute_newly_requested_permissions(manifest, current_manifest_for_update);
    bool is_update = current_manifest_for_update.has_value();
    bool requires_prompt = !is_update || !newly_requested.is_empty();

    StagedInstall staged {
        .token = token,
        .addon_id = result.addon_id,
        .name = manifest.name(),
        .version = result.version,
        .sha256_hex = result.sha256_hex,
        .manifest_json = result.manifest_json,
        .staged_directory = stage_dir,
        .icon_path = manifest.package_icon_path(),
        .permissions = {
            .permissions = manifest.permissions(),
            .host_permissions = collect_host_permissions(manifest),
            .optional_permissions = manifest.optional_permissions(),
            .optional_host_permissions = move(opt_host_perms),
            .data_collection = manifest.data_collection_permissions(),
        },
        .compatibility_status = result.compatibility_status,
        .compatibility_reasons = result.compatibility_reasons,
        .verified_signature = result.verified_signature,
        .signer_cn = result.signer_cn,
        .is_update = is_update,
        .requires_permission_prompt = requires_prompt,
        .newly_requested_permissions = move(newly_requested),
        .previous_sha256_hex = previous_sha256_hex,
        .from_catalog = expected_sha256_hex.has_value(),
    };

    m_staged.set(token, staged);
    keep_stage_dir = true;
    return staged;
}

ErrorOr<void> ExtensionInstaller::cancel_install(StringView token)
{
    auto staged = m_staged.take(token);
    if (!staged.has_value())
        return {};
    if (FileSystem::exists(staged->staged_directory))
        TRY(FileSystem::remove(staged->staged_directory, FileSystem::RecursionMode::Allowed));
    return {};
}

ErrorOr<CommitResult> ExtensionInstaller::commit_staged(
    StringView token,
    bool user_consented,
    Optional<ByteString> const& previous_install_directory,
    Function<ErrorOr<void>(CommitResult const&)> const& on_commit_registry)
{
    auto it = m_staged.find(token);
    if (it == m_staged.end())
        return Error::from_string_literal("Unknown or expired staged install token");

    auto staged = it->value;
    if (staged.requires_permission_prompt && !user_consented)
        return Error::from_string_literal("Expanded permissions require explicit user consent");
    if (m_revocation_list.is_revoked(staged.addon_id, staged.version, staged.sha256_hex))
        return Error::from_string_literal("Extension is revoked");

    m_staged.remove(token);
    auto dest_dir = LexicalPath::join(installed_root(), staged.token).string();
    bool moved_to_destination = false;

    ArmedScopeGuard rollback_guard = [&] {
        if (moved_to_destination)
            (void)FileSystem::remove(dest_dir, FileSystem::RecursionMode::Allowed);
        if (FileSystem::exists(staged.staged_directory))
            (void)FileSystem::remove(staged.staged_directory, FileSystem::RecursionMode::Allowed);
    };

    TRY(Core::Directory::create(installed_root(), Core::Directory::CreateDirectories::Yes));
    if (FileSystem::exists(dest_dir))
        return Error::from_string_literal("Extension install directory already exists");
    TRY(Core::System::rename(staged.staged_directory, dest_dir));
    moved_to_destination = true;

    if (m_fail_next_commit_for_testing) {
        m_fail_next_commit_for_testing = false;
        return Error::from_string_literal("Simulated commit failure for rollback testing");
    }

    CommitResult commit {
        .addon_id = move(staged.addon_id),
        .version = move(staged.version),
        .install_directory = LexicalPath::join(dest_dir, "payload"sv).string(),
        .sha256_hex = move(staged.sha256_hex),
        .manifest_json = move(staged.manifest_json),
        .compatibility_status = staged.compatibility_status,
    };

    if (on_commit_registry)
        TRY(on_commit_registry(commit));

    rollback_guard.disarm();
    if (previous_install_directory.has_value())
        remove_installed_package(*previous_install_directory);

    return commit;
}

void ExtensionInstaller::remove_installed_package(ByteString const& payload_directory) const
{
    auto package_directory = LexicalPath::dirname(payload_directory);
    if (LexicalPath::basename(payload_directory) != "payload"sv || LexicalPath::dirname(package_directory) != installed_root())
        return;
    (void)FileSystem::remove(package_directory, FileSystem::RecursionMode::Allowed);
}

ErrorOr<Web::Extensions::ExtensionManifest> ExtensionInstaller::validate_installed_package(
    ByteString const& payload_directory, String const& addon_id, String const& sha256_hex)
{
    auto package = LexicalPath::dirname(payload_directory);
    if (LexicalPath::basename(payload_directory) != "payload"sv || LexicalPath::dirname(package) != installed_root())
        return Error::from_string_literal("Installed package path is outside the extension profile");
    auto root = TRY(FileSystem::real_path(installed_root()));
    auto canonical_payload = TRY(FileSystem::real_path(payload_directory));
    if (canonical_payload != LexicalPath::join(root, LexicalPath::basename(package), "payload"sv).string())
        return Error::from_string_literal("Installed package path contains an unexpected filesystem link");

    auto archive_path = LexicalPath::join(package, "package.xpi"sv).string();
    auto verified = TRY(verify_package(TRY(read_extension_file(archive_path, max_package_size)), addon_id));
    if (!verified.metadata.sha256_hex.equals_ignoring_ascii_case(sha256_hex))
        return Error::from_string_literal("Installed package checksum differs from the approved extension");

    HashMap<ByteString, size_t> expected_files;
    for (size_t index = 0; index < verified.file_names.size(); ++index)
        expected_files.set(LexicalPath::join(canonical_payload, verified.file_names[index]).string(), index);

    Vector<ByteString> directories;
    directories.append(canonical_payload);
    size_t directory_count = 1;
    while (!directories.is_empty()) {
        Core::DirIterator iterator(directories.take_last(), Core::DirIterator::SkipDots);
        while (iterator.has_next()) {
            auto path = iterator.next_full_path();
            auto metadata = TRY(Core::System::lstat(path));
            if (S_ISDIR(metadata.st_mode)) {
                if (++directory_count > Web::Extensions::ZipReader::max_entries)
                    return Error::from_string_literal("Installed package contains too many directories");
                directories.append(move(path));
                continue;
            }
            auto expected = expected_files.take(path);
            if (!S_ISREG(metadata.st_mode) || !expected.has_value())
                return Error::from_string_literal("Installed package contains an unexpected file");
            auto const& signed_contents = verified.file_contents[*expected];
            auto installed_contents = TRY(read_extension_file(path, signed_contents.size()));
            if (signed_contents.bytes() != installed_contents.bytes())
                return Error::from_string_literal("Installed extension file no longer matches its signed package");
        }
        if (iterator.has_error())
            return iterator.error();
    }
    if (!expected_files.is_empty())
        return Error::from_string_literal("Installed package is missing a signed file");
    return Web::Extensions::ExtensionManifest::parse(verified.metadata.manifest_json);
}

Optional<StagedInstall const&> ExtensionInstaller::staged_install(StringView token) const
{
    return m_staged.get(token);
}

}
