/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/File.h>
#include <LibFileSystem/FileSystem.h>
#include <LibTest/TestCase.h>
#include <LibWebView/FaviconStore.h>
#include <QTemporaryDir>
#include <UI/Qt/BookmarkStore.h>
#include <UI/Qt/StringUtils.h>

TEST_CASE(fresh_bookmark_store_seeds_ladyflock_defaults)
{
    QTemporaryDir temp_directory;
    VERIFY(temp_directory.isValid());
    auto bookmarks_path = ByteString::formatted("{}/Bookmarks.json", ak_byte_string_from_qstring(temp_directory.path()));
    auto favicon_store = WebView::FaviconStore::create();

    auto store = Ladybird::BookmarkStore::create(bookmarks_path, *favicon_store);
    VERIFY(FileSystem::exists(bookmarks_path));
    VERIFY(store.root_items().size() == 3);

    EXPECT(store.root_items()[0].is_bookmark());
    EXPECT_EQ(store.root_items()[0].bookmark().title, "Ladyflock"_string);
    EXPECT_EQ(store.root_items()[0].bookmark().url.serialize(), "https://github.com/Kayyo321/Ladyflock"sv);

    EXPECT(store.root_items()[1].is_bookmark());
    EXPECT_EQ(store.root_items()[1].bookmark().title, "Ladyflock Issues"_string);
    EXPECT_EQ(store.root_items()[1].bookmark().url.serialize(), "https://github.com/Kayyo321/Ladyflock/issues"sv);

    EXPECT(store.root_items()[2].is_bookmark());
    EXPECT_EQ(store.root_items()[2].bookmark().title, "Ladybird Engine"_string);
    EXPECT_EQ(store.root_items()[2].bookmark().url.serialize(), "https://ladybird.org/"sv);
}

TEST_CASE(existing_and_empty_bookmark_stores_are_not_overwritten)
{
    QTemporaryDir temp_directory;
    VERIFY(temp_directory.isValid());
    auto dir_path = ak_byte_string_from_qstring(temp_directory.path());
    auto favicon_store = WebView::FaviconStore::create();

    {
        auto empty_path = ByteString::formatted("{}/EmptyBookmarks.json", dir_path);
        auto file = MUST(Core::File::open(empty_path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(R"({"version":2,"items":[]})"sv.bytes()));
        file->close();

        auto store = Ladybird::BookmarkStore::create(empty_path, *favicon_store);
        EXPECT(store.root_items().is_empty());
    }

    {
        auto existing_path = ByteString::formatted("{}/ExistingBookmarks.json", dir_path);
        auto file = MUST(Core::File::open(existing_path, Core::File::OpenMode::Write));
        MUST(file->write_until_depleted(R"({"version":2,"items":[{"type":"bookmark","id":"custom-1","url":"https://example.org/","title":"Example","date_added":1,"last_modified":1}]})"sv.bytes()));
        file->close();

        auto store = Ladybird::BookmarkStore::create(existing_path, *favicon_store);
        VERIFY(store.root_items().size() == 1);
        EXPECT_EQ(store.root_items()[0].bookmark().title, "Example"_string);
        EXPECT_EQ(store.root_items()[0].bookmark().url.serialize(), "https://example.org/"sv);
    }
}
