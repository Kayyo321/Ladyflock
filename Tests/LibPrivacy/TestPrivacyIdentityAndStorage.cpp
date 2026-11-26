/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Message.h>
#include <LibPrivacy/Identity/NavigationHandoff.h>
#include <LibPrivacy/Identity/Types.h>
#include <LibPrivacy/Storage/PartitionKey.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>

TEST_CASE(identity_descriptor_and_domain_rules)
{
    EXPECT(Privacy::is_valid_identity_id("default"sv));
    EXPECT(Privacy::is_valid_identity_id("work-1_alpha"sv));
    EXPECT(!Privacy::is_valid_identity_id(""sv));
    EXPECT(!Privacy::is_valid_identity_id("../escape"sv));
    EXPECT(!Privacy::is_valid_identity_id("UPPER"sv));

    auto default_id = Privacy::IdentityDescriptor::default_persistent();
    EXPECT(default_id.is_valid());
    EXPECT(default_id.is_default_identity());
    EXPECT(!default_id.is_ephemeral());

    auto temp = Privacy::IdentityDescriptor::temporary("temp1"_string, "Temp"_string);
    EXPECT(temp.is_valid());
    EXPECT(temp.is_ephemeral());
    EXPECT(!temp.persist_history);

    auto bad_offline = Privacy::IdentityDescriptor::offline("off1"_string, "Offline"_string);
    EXPECT(bad_offline.is_valid());
    bad_offline.network_mode = Privacy::NetworkMode::Allow;
    EXPECT(!bad_offline.is_valid());

    Privacy::DomainAssignmentRule wildcard { .pattern = "*.work.example"_string, .target_identity_id = "work"_string };
    EXPECT(wildcard.is_valid());
    EXPECT(wildcard.matches_host("work.example"sv));
    EXPECT(wildcard.matches_host("portal.work.example"sv));
    EXPECT(wildcard.matches_host("a.b.WORK.EXAMPLE."sv));
    EXPECT(!wildcard.matches_host("notwork.example"sv));
    EXPECT(!wildcard.matches_host("example.com"sv));

    Privacy::DomainAssignmentRule exact { .pattern = "bank.example"_string, .target_identity_id = "finance"_string };
    EXPECT(exact.is_valid());
    EXPECT(exact.matches_host("bank.example"sv));
    EXPECT(!exact.matches_host("sub.bank.example"sv));
}

TEST_CASE(navigation_handoff_confirmation_and_sanitization)
{
    auto offline_id = Privacy::IdentityDescriptor::offline("offline"_string, "Offline"_string);
    auto work_id = Privacy::IdentityDescriptor::persistent("work"_string, "Work"_string);
    auto dest_url = URL::Parser::basic_parse("https://portal.work.example/app?token=123"sv).release_value();

    // Same identity stays in place.
    {
        auto decision = Privacy::NavigationHandoffEvaluator::evaluate({
            .source_identity = work_id,
            .source_policy = Privacy::NetworkPolicy::allow_default(),
            .target_identity = work_id,
            .target_policy = Privacy::NetworkPolicy::allow_default(),
            .destination_url = dest_url,
            .http_method = "POST",
            .has_post_body = true,
            .has_opener = true,
            .has_referrer = true,
            .has_credentials = true,
            .user_confirmed = false,
        });
        EXPECT_EQ(decision.action, Privacy::HandoffAction::StayInCurrentIdentity);
        EXPECT(!decision.open_fresh_context);
        EXPECT(decision.replay_post_body);
    }

    // Crossing from Offline -> Allow without confirmation MUST pause and ask user while displaying destination URL.
    {
        auto decision = Privacy::NavigationHandoffEvaluator::evaluate({
            .source_identity = offline_id,
            .source_policy = Privacy::NetworkPolicy::offline(),
            .target_identity = work_id,
            .target_policy = Privacy::NetworkPolicy::allow_default(),
            .destination_url = dest_url,
            .http_method = "POST",
            .has_post_body = true,
            .has_opener = true,
            .has_referrer = true,
            .has_credentials = true,
            .user_confirmed = false,
        });
        EXPECT_EQ(decision.action, Privacy::HandoffAction::RequiresConfirmation);
        EXPECT_EQ(decision.displayed_destination_url, "https://portal.work.example/app?token=123"sv);
        EXPECT(!decision.replay_post_body);
    }

    // With explicit confirmation, handoff opens a fresh context and strips opener, referrer, credentials, and POST body.
    {
        auto decision = Privacy::NavigationHandoffEvaluator::evaluate({
            .source_identity = offline_id,
            .source_policy = Privacy::NetworkPolicy::offline(),
            .target_identity = work_id,
            .target_policy = Privacy::NetworkPolicy::allow_default(),
            .destination_url = dest_url,
            .http_method = "POST",
            .has_post_body = true,
            .has_opener = true,
            .has_referrer = true,
            .has_credentials = true,
            .user_confirmed = true,
        });
        EXPECT_EQ(decision.action, Privacy::HandoffAction::HandoffToNewContext);
        EXPECT_EQ(decision.target_identity_id, "work"sv);
        EXPECT_EQ(decision.displayed_destination_url, "https://portal.work.example/app?token=123"sv);
        EXPECT(decision.open_fresh_context);
        EXPECT(decision.drop_opener);
        EXPECT(decision.strip_referrer);
        EXPECT(decision.strip_credentials);
        EXPECT(!decision.replay_post_body);
    }

    // Crossing from Allow -> Offline (more restrictive) does not require confirmation but still sanitizes the boundary.
    {
        auto decision = Privacy::NavigationHandoffEvaluator::evaluate({
            .source_identity = work_id,
            .source_policy = Privacy::NetworkPolicy::allow_default(),
            .target_identity = offline_id,
            .target_policy = Privacy::NetworkPolicy::offline(),
            .destination_url = dest_url,
            .http_method = "GET",
            .has_post_body = false,
            .has_opener = true,
            .has_referrer = true,
            .has_credentials = true,
            .user_confirmed = false,
        });
        EXPECT_EQ(decision.action, Privacy::HandoffAction::HandoffToNewContext);
        EXPECT(decision.open_fresh_context);
        EXPECT(decision.drop_opener);
        EXPECT(decision.strip_referrer);
        EXPECT(decision.strip_credentials);
        EXPECT(!decision.replay_post_body);
    }
}

TEST_CASE(storage_partition_key_and_legacy_classification)
{
    auto site_a = URL::Parser::basic_parse("https://site-a.example:8443/page"sv).release_value().origin();
    auto site_b = URL::Parser::basic_parse("https://site-b.example:8443/page"sv).release_value().origin();
    auto embed = URL::Parser::basic_parse("https://embed.example:8443/frame"sv).release_value().origin();

    // First-party partition key for site_a.
    auto fp_a = Privacy::StoragePartitionKey::from_context(site_a, site_a, false);
    VERIFY(fp_a.has_value());
    EXPECT_EQ(fp_a->to_storage_key_string(), "p1|https://site-a.example:8443|https://site-a.example|0"sv);

    // Same-origin embed under site_a vs site_b get distinct partition keys.
    auto embed_under_a = Privacy::StoragePartitionKey::from_context(embed, site_a, true);
    auto embed_under_b = Privacy::StoragePartitionKey::from_context(embed, site_b, true);
    VERIFY(embed_under_a.has_value());
    VERIFY(embed_under_b.has_value());
    EXPECT_NE(embed_under_a->to_storage_key_string(), embed_under_b->to_storage_key_string());

    // A -> B -> A (same-site top-level with cross-site ancestor) is isolated from first-party A.
    auto a_nested_in_b = Privacy::StoragePartitionKey::from_context(site_a, site_a, true);
    VERIFY(a_nested_in_b.has_value());
    EXPECT_NE(a_nested_in_b->to_storage_key_string(), fp_a->to_storage_key_string());

    // Ordinary opaque origin and opaque file:// origin cannot obtain a persistent storage key by default.
    auto opaque = URL::Origin::create_opaque();
    EXPECT(!Privacy::StoragePartitionKey::from_context(opaque, site_a, false).has_value());
    auto opaque_file = URL::Parser::basic_parse("file:///tmp/test.html"sv).release_value().origin();
    EXPECT(!Privacy::StoragePartitionKey::from_context(opaque_file, opaque_file, false).has_value());
    EXPECT(!Privacy::StoragePartitionKey::from_context(site_a, opaque, false).has_value());

    // Roundtrip parse.
    auto parsed = Privacy::StoragePartitionKey::parse(fp_a->to_storage_key_string());
    VERIFY(parsed.has_value());
    EXPECT(*parsed == *fp_a);

    // Legacy classification.
    auto legacy_http = Privacy::classify_legacy_storage_key("https://app.example:8443"sv);
    EXPECT_EQ(legacy_http.action, Privacy::LegacyStorageKeyAction::MigrateToFirstParty);
    EXPECT_EQ(legacy_http.migrated_storage_key, "p1|https://app.example:8443|https://app.example|0"_string);

    auto already = Privacy::classify_legacy_storage_key("p1|https://app.example:8443|https://app.example|0"sv);
    EXPECT_EQ(already.action, Privacy::LegacyStorageKeyAction::AlreadyPartitioned);

    auto legacy_file = Privacy::classify_legacy_storage_key("file://"sv);
    EXPECT_EQ(legacy_file.action, Privacy::LegacyStorageKeyAction::Quarantine);
    EXPECT_EQ(legacy_file.quarantine_reason, "legacy-file-origin-exception"sv);

    auto legacy_null = Privacy::classify_legacy_storage_key("null"sv);
    EXPECT_EQ(legacy_null.action, Privacy::LegacyStorageKeyAction::Quarantine);
}
