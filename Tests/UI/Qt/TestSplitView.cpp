/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <UI/Qt/HiddenPageSizing.h>
#include <UI/Qt/SplitView.h>

#include <QApplication>
#include <QKeyEvent>
#include <QVBoxLayout>

using namespace Ladybird;

// Both sides show from 2 × 320 + 8 px, and once they've given way to one side, they only return 24 px past that, so the
// layout doesn't flap while the window's edge is dragged across it.
TEST_CASE(sides_show_side_by_side_with_hysteresis)
{
    EXPECT_EQ(SplitView::side_by_side_threshold(), 648);

    EXPECT(SplitView::shows_side_by_side(648, true));
    EXPECT(!SplitView::shows_side_by_side(647, true));

    EXPECT(!SplitView::shows_side_by_side(648, false));
    EXPECT(!SplitView::shows_side_by_side(671, false));
    EXPECT(SplitView::shows_side_by_side(672, false));
}

TEST_CASE(a_missing_or_out_of_range_fraction_is_the_middle)
{
    EXPECT_EQ(SplitView::sanitized_start_fraction({}), 0.5f);
    EXPECT_EQ(SplitView::sanitized_start_fraction(0.0f), 0.5f);
    EXPECT_EQ(SplitView::sanitized_start_fraction(1.0f), 0.5f);
    EXPECT_EQ(SplitView::sanitized_start_fraction(-0.2f), 0.5f);
    EXPECT_EQ(SplitView::sanitized_start_fraction(__builtin_nanf("")), 0.5f);
    EXPECT_EQ(SplitView::sanitized_start_fraction(__builtin_inff()), 0.5f);
    EXPECT_EQ(SplitView::sanitized_start_fraction(0.35f), 0.35f);
}

// The share of the width is shared by the two pages, without the gutter between them.
TEST_CASE(side_widths_share_the_room_without_the_gutter)
{
    auto widths = SplitView::side_widths(1208, 0.5f);
    EXPECT_EQ(widths.start, 600);
    EXPECT_EQ(widths.end, 600);

    widths = SplitView::side_widths(1208, 0.35f);
    EXPECT_EQ(widths.start, 420);
    EXPECT_EQ(widths.end, 780);
    EXPECT_EQ(widths.start + widths.end + SplitView::DIVIDER_WIDTH, 1208);
}

// The person's preferred share stays as it is, and only what shows is held to each side's minimum.
TEST_CASE(displayed_widths_keep_each_side_at_least_320_px)
{
    auto widths = SplitView::side_widths(808, 0.1f);
    EXPECT_EQ(widths.start, 320);
    EXPECT_EQ(widths.end, 480);

    widths = SplitView::side_widths(808, 0.95f);
    EXPECT_EQ(widths.start, 480);
    EXPECT_EQ(widths.end, 320);

    widths = SplitView::side_widths(648, 0.2f);
    EXPECT_EQ(widths.start, 320);
    EXPECT_EQ(widths.end, 320);
}

TEST_CASE(a_released_divider_snaps_to_the_middle_within_12_px)
{
    // 1208 px leaves 1200 px for the pages, so the middle is at 600.
    EXPECT_EQ(SplitView::start_fraction_for_divider_position(612, 1208), 0.5f);
    EXPECT_EQ(SplitView::start_fraction_for_divider_position(588, 1208), 0.5f);
    EXPECT_EQ(SplitView::start_fraction_for_divider_position(613, 1208), 613.0f / 1200.0f);
    // A divider dragged past a side's minimum stops there.
    EXPECT_EQ(SplitView::start_fraction_for_divider_position(100, 1208), 320.0f / 1200.0f);
    EXPECT_EQ(SplitView::start_fraction_for_divider_position(1150, 1208), 880.0f / 1200.0f);
}

static Vector<float> committed_fractions_for_keys(Vector<QKeyCombination> keys, Qt::LayoutDirection direction = Qt::LeftToRight, float start_fraction = 0.5f)
{
    QWidget window;
    auto* page_area = new PageArea(&window);
    page_area->setLayoutDirection(direction);
    page_area->resize(1208, 600);
    auto& divider = page_area->divider();
    auto widths = SplitView::side_widths(1208, start_fraction);
    divider.set_split_geometry(0, 1208, widths.start, start_fraction, direction == Qt::RightToLeft);

    Vector<float> fractions;
    QObject::connect(&divider, &SideDivider::start_fraction_committed, &divider, [&](float fraction) { fractions.append(fraction); });
    for (auto key : keys) {
        QKeyEvent event(QEvent::KeyPress, key.key(), key.keyboardModifiers());
        QApplication::sendEvent(&divider, &event);
    }
    return fractions;
}

static bool is_close(float a, float b)
{
    return fabsf(a - b) < 0.0001f;
}

// The divider is a keyboard stop: the arrows move it by 5 %, or 1 % with Shift, Home and End move it to the limits and
// Return puts it back in the middle.
TEST_CASE(the_divider_moves_with_the_keyboard)
{
    auto fractions = committed_fractions_for_keys({ Qt::Key_Right, Qt::SHIFT | Qt::Key_Left, Qt::Key_Home, Qt::Key_End, Qt::Key_Return });
    EXPECT_EQ(fractions.size(), 5u);
    EXPECT(is_close(fractions[0], 0.55f));
    // NB: Each commit takes the divider's new place as its own, so the next key moves on from there.
    EXPECT(is_close(fractions[1], 0.54f));
    EXPECT(is_close(fractions[2], 320.0f / 1200.0f));
    EXPECT(is_close(fractions[3], 880.0f / 1200.0f));
    EXPECT(is_close(fractions[4], 0.5f));
}

// In a right-to-left layout the start side is on the right, and the arrow toward the end still moves the divider
// toward the end.
TEST_CASE(the_divider_arrows_follow_the_layout_direction)
{
    auto fractions = committed_fractions_for_keys({ Qt::Key_Left }, Qt::RightToLeft);
    EXPECT_EQ(fractions.size(), 1u);
    EXPECT(is_close(fractions[0], 0.55f));
}

TEST_CASE(the_divider_is_an_accessible_separator)
{
    QWidget window;
    auto* page_area = new PageArea(&window);
    EXPECT_EQ(page_area->divider().accessibleName(), QStringLiteral("Split view divider"));
    EXPECT_EQ(page_area->divider().focusPolicy(), Qt::TabFocus);
}

// A hidden page of a pair is sized like its side, not like the whole area, so it loads into the width it will show at.
TEST_CASE(a_hidden_page_takes_the_size_of_its_slot)
{
    QWidget window;
    auto* page = new QWidget(&window);
    auto* child = new QWidget(page);
    auto* layout = new QVBoxLayout(page);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(child);
    page->hide();

    window.resize(1208, 700);
    window.show();
    QApplication::processEvents();

    auto widths = SplitView::side_widths(1208, 0.35f);
    size_hidden_page_to(*page, QSize(widths.start, 700));
    EXPECT(!page->isVisible());
    EXPECT_EQ(page->size(), QSize(420, 700));
    EXPECT_EQ(child->size(), QSize(420, 700));
}
