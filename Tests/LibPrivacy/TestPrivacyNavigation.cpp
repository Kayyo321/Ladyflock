/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibPrivacy/Navigation/TrackingParameters.h>
#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>

static ByteString stripped(StringView input)
{
    auto url = URL::Parser::basic_parse(input).release_value();
    auto result = Privacy::strip_tracking_parameters(url);
    return result.has_value() ? result->serialize().to_byte_string() : url.serialize().to_byte_string();
}

TEST_CASE(tracking_parameter_names)
{
    for (auto name : { "utm_source"sv, "UTM_Campaign"sv, "utm_"sv, "utm%5Fmedium"sv, "fbclid"sv, "gclid"sv, "dclid"sv, "gbraid"sv,
             "wbraid"sv, "srsltid"sv, "msclkid"sv, "_ga"sv, "mc_eid"sv, "yclid"sv }) {
        EXPECT(Privacy::is_tracking_query_parameter(name));
    }
    // Names that only look alike are the site's own, and a cache buster is not a tracker.
    for (auto name : { "utmost"sv, "utm"sv, "xutm_source"sv, "fbclid2"sv, "gclid_"sv, "code"sv, "state"sv, "id"sv, "zx"sv, ""sv }) {
        EXPECT(!Privacy::is_tracking_query_parameter(name));
    }
}

TEST_CASE(tracking_parameters_come_off_and_everything_else_stays_as_written)
{
    // Campaign tags and click identifiers are removed; the page's own parameters, their order and the fragment stay.
    EXPECT_EQ(stripped("https://shop.example/item?utm_source=news&id=7&fbclid=abc&color=red#reviews"sv), "https://shop.example/item?id=7&color=red#reviews"sv);

    // A query of nothing but tracking parameters goes away entirely, question mark included.
    EXPECT_EQ(stripped("https://news.example/story?utm_source=a&utm_medium=b&gclid=c"sv), "https://news.example/story"sv);
    EXPECT_EQ(stripped("https://news.example/story?gclid=c#top"sv), "https://news.example/story#top"sv);

    // Nothing to remove leaves the URL alone.
    EXPECT(!Privacy::strip_tracking_parameters(URL::Parser::basic_parse("https://a.example/?q=1"sv).release_value()).has_value());
    EXPECT(!Privacy::strip_tracking_parameters(URL::Parser::basic_parse("https://a.example/"sv).release_value()).has_value());
    EXPECT(!Privacy::strip_tracking_parameters(URL::Parser::basic_parse("https://a.example/?"sv).release_value()).has_value());

    // Only web URLs are rewritten.
    EXPECT(!Privacy::strip_tracking_parameters(URL::Parser::basic_parse("file:///tmp/a.html?utm_source=x"sv).release_value()).has_value());
    EXPECT(!Privacy::strip_tracking_parameters(URL::Parser::basic_parse("about:blank?utm_source=x"sv).release_value()).has_value());

    size_t removed = 0;
    EXPECT(Privacy::strip_tracking_parameters(URL::Parser::basic_parse("https://a.example/?utm_a=1&b=2&utm_c=3"sv).release_value(), &removed).has_value());
    EXPECT_EQ(removed, 2u);
}

TEST_CASE(sign_in_callbacks_and_signed_urls_survive_byte_for_byte)
{
    // An OAuth callback carries nothing that looks like tracking, so it is not touched.
    EXPECT_EQ(stripped("https://app.example/callback?code=4%2F0Ab_x&state=s%3D1%26t%3D2"sv), "https://app.example/callback?code=4%2F0Ab_x&state=s%3D1%26t%3D2"sv);

    // A signed URL keeps every signed parameter in its exact encoding and order, and only the tag that was added to it
    // afterwards comes off.
    EXPECT_EQ(stripped("https://cdn.example/f.bin?X-Amz-Credential=AKIA%2F20260101%2Fus-east-1&X-Amz-Signature=a%2Bb%3D&utm_campaign=z&X-Amz-Expires=60"sv),
        "https://cdn.example/f.bin?X-Amz-Credential=AKIA%2F20260101%2Fus-east-1&X-Amz-Signature=a%2Bb%3D&X-Amz-Expires=60"sv);

    // Duplicate keys, empty values, empty segments and bare names keep their exact shape.
    EXPECT_EQ(stripped("https://a.example/?a=1&a=2&utm_x=3&&flag&b="sv), "https://a.example/?a=1&a=2&&flag&b="sv);
    EXPECT_EQ(stripped("https://a.example/?utmost=1&utm=2&fbclid"sv), "https://a.example/?utmost=1&utm=2"sv);
    // A parameter whose value merely contains a tracking name is the site's own.
    EXPECT_EQ(stripped("https://a.example/?next=%2F%3Futm_source%3Dx&gclid=1"sv), "https://a.example/?next=%2F%3Futm_source%3Dx"sv);
}

TEST_CASE(stripping_cost)
{
    // Not an assertion on speed: this records the cost per URL on the machine that runs the suite.
    auto with_tracking = URL::Parser::basic_parse("https://shop.example/item?utm_source=news&utm_medium=mail&id=7&fbclid=abc&color=red#reviews"sv).release_value();
    auto without_tracking = URL::Parser::basic_parse("https://shop.example/item?id=7&color=red&size=m#reviews"sv).release_value();
    static constexpr size_t iterations = 100'000;

    for (auto const* url : { &with_tracking, &without_tracking }) {
        auto start = MonotonicTime::now();
        size_t changed = 0;
        for (size_t i = 0; i < iterations; ++i)
            changed += Privacy::strip_tracking_parameters(*url).has_value() ? 1 : 0;
        auto elapsed = MonotonicTime::now() - start;
        outln("strip_tracking_parameters ({} tracking): {} ns per URL over {} iterations",
            changed ? "with"sv : "without"sv, elapsed.to_nanoseconds() / static_cast<i64>(iterations), iterations);
        EXPECT_EQ(changed, url == &with_tracking ? iterations : 0u);
    }
}
