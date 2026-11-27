/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Complete 30-widget catalog and presets matching TabsNSuch.md §5 & §7.
 */

// Every section, like everything else on the board, is colored by the browser's accent,
// so sections are told apart by their glyphs and names rather than by hue.
export const CATALOG_SECTIONS = [
    { id: "navigation", title: "Navigation, tabs and sessions", short: "Navigate" },
    { id: "activity", title: "Activity and library", short: "Library" },
    { id: "privacy", title: "Privacy and browser controls", short: "Privacy" },
    { id: "productivity", title: "Time and personal productivity", short: "Productivity" },
    { id: "media", title: "Media and weather", short: "Media" },
    { id: "developer", title: "Developer tools and references", short: "Developer" },
];

// Names shown on a tile's eyebrow where the catalog title is too long to sit beside the status text.
const SHORT_TITLES = {
    "workspaces": "Workspaces",
    "permissions-audit": "Permissions",
    "profile-switcher": "Profile",
    "calendar-countdown": "Calendar",
    "calculator": "Calculator",
    "color-contrast": "Contrast",
    "dev-codec": "Codec",
    "shortcuts-guide": "Shortcuts",
    "pinned-references": "References",
};

export const SIZE_NAMES = { compact: "Small", default: "Medium", expanded: "Large" };

export function getWidgetShortTitle(def) {
    if (!def)
        return "";
    return SHORT_TITLES[def.id] || def.title;
}

function sz(w, h, tier) {
    return { w, h, tier, label: `${w}×${h}` };
}

export const WIDGET_CATALOG = [
    // 1. Navigation, tabs and sessions
    {
        num: 1,
        id: "top-sites",
        title: "Top Sites",
        section: "navigation",
        sizes: [sz(2, 1, "compact"), sz(4, 2, "default"), sz(8, 3, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Pinned shortcuts and your most visited sites.",
        sample: "Ladybird · MDN · GitHub · Docs",
        defaultConfig: () => ({}),
    },
    {
        num: 2,
        id: "search-box",
        title: "Search Box",
        section: "navigation",
        sizes: [sz(4, 1, "compact"), sz(6, 1, "default"), sz(8, 2, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Search the web with your configured engine without suggestions.",
        sample: "Search with default engine…",
        defaultConfig: () => ({}),
    },
    {
        num: 3,
        id: "open-tabs",
        title: "Open Tabs",
        section: "navigation",
        sizes: [sz(3, 2, "compact"), sz(4, 2, "default"), sz(6, 4, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Switch to, mute, or close tabs in this session.",
        sample: "3 tabs open in this window",
        defaultConfig: () => ({}),
    },
    {
        num: 4,
        id: "recently-closed",
        title: "Recently Closed",
        section: "navigation",
        sizes: [sz(3, 1, "compact"), sz(4, 2, "default"), sz(6, 3, "expanded")],
        multi: false,
        network: false,
        privateAllowed: false,
        description: "Reopen recently closed tabs and windows.",
        sample: "Reopen last closed tab",
        defaultConfig: () => ({}),
    },
    {
        num: 5,
        id: "workspaces",
        title: "Saved Workspaces",
        section: "navigation",
        sizes: [sz(2, 2, "compact"), sz(4, 2, "default"), sz(6, 4, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Save a window’s web tabs as a named set and reopen them in order.",
        sample: "Research (6 tabs) · Release review (4 tabs)",
        defaultConfig: () => ({}),
    },
    {
        num: 6,
        id: "bookmark-shelf",
        title: "Bookmark Shelf",
        section: "navigation",
        sizes: [sz(2, 1, "compact"), sz(4, 2, "default"), sz(8, 4, "expanded")],
        multi: true,
        network: false,
        privateAllowed: false,
        description: "Keep one bookmark folder within reach.",
        sample: "Bookmarks Bar · 5 links",
        defaultConfig: () => ({ folderId: "root" }),
    },

    // 2. Activity and library
    {
        num: 7,
        id: "recent-history",
        title: "Recent History",
        section: "activity",
        sizes: [sz(3, 2, "compact"), sz(4, 3, "default"), sz(6, 4, "expanded")],
        multi: false,
        network: false,
        privateAllowed: false,
        description: "Browse, filter, and remove recent pages.",
        sample: "Today · Web Standards · Release notes",
        defaultConfig: () => ({}),
    },
    {
        num: 8,
        id: "downloads",
        title: "Downloads",
        section: "activity",
        sizes: [sz(3, 1, "compact"), sz(4, 2, "default"), sz(6, 4, "expanded")],
        multi: false,
        network: false,
        privateAllowed: false,
        description: "Track progress, pause/resume, or reveal finished transfers.",
        sample: "archive.tar.gz · Completed",
        defaultConfig: () => ({}),
    },
    {
        num: 9,
        id: "reading-queue",
        title: "Reading Queue",
        section: "activity",
        sizes: [sz(3, 2, "compact"), sz(4, 2, "default"), sz(6, 4, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Save articles to read later, offline, with tags and notes.",
        sample: "2 unread articles",
        defaultConfig: () => ({}),
    },
    {
        num: 10,
        id: "feed-reader",
        title: "Feed Reader",
        section: "activity",
        sizes: [sz(3, 2, "compact"), sz(4, 3, "default"), sz(8, 4, "expanded")],
        multi: false,
        network: true,
        privateAllowed: false,
        description: "Read RSS and Atom feeds after explicit per-source HTTPS consent.",
        sample: "Opt-in HTTPS feeds · Manual refresh default",
        defaultConfig: () => ({ sourceIds: [] }),
    },

    // 3. Privacy and browser controls
    {
        num: 11,
        id: "privacy-summary",
        title: "Privacy Summary",
        section: "privacy",
        sizes: [sz(2, 1, "compact"), sz(4, 2, "default"), sz(6, 3, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "See effective network mode, sanitized navigations, and routing policy.",
        sample: "Allow · 0 tracking params stripped",
        defaultConfig: () => ({}),
    },
    {
        num: 12,
        id: "extensions-dock",
        title: "Extensions Dock",
        section: "privacy",
        sizes: [sz(2, 1, "compact"), sz(4, 2, "default"), sz(6, 3, "expanded")],
        multi: false,
        network: false,
        privateAllowed: false,
        description: "View installed extensions, toggle status, and open options.",
        sample: "Installed add-ons · Ordinary session",
        defaultConfig: () => ({}),
    },
    {
        num: 13,
        id: "permissions-audit",
        title: "Site Permissions",
        section: "privacy",
        sizes: [sz(3, 2, "compact"), sz(4, 2, "default"), sz(6, 4, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Review and revoke site permission grants in this session.",
        sample: "Geolocation · Microphone · Notifications",
        defaultConfig: () => ({}),
    },
    {
        num: 14,
        id: "profile-switcher",
        title: "Profile and Identity",
        section: "privacy",
        sizes: [sz(2, 1, "compact"), sz(3, 2, "default"), sz(4, 2, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Identify current profile and browsing identity; launch private windows.",
        sample: "Default profile · Standard identity",
        defaultConfig: () => ({}),
    },
    {
        num: 15,
        id: "browser-health",
        title: "Browser Health",
        section: "privacy",
        sizes: [sz(2, 1, "compact"), sz(4, 2, "default"), sz(6, 3, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Inspect tab CPU/memory samples and discard idle background tabs.",
        sample: "Eligible tabs · Per-tab samples",
        defaultConfig: () => ({}),
    },

    // 4. Time and personal productivity
    {
        num: 16,
        id: "world-clock",
        title: "World Clock",
        section: "productivity",
        sizes: [sz(2, 1, "compact"), sz(3, 2, "default"), sz(6, 3, "expanded")],
        multi: true,
        network: false,
        privateAllowed: true,
        description: "Local time, date, and labelled IANA time zones.",
        sample: "10:15 AM · UTC · Tokyo · London",
        defaultConfig: () => ({ use24Hour: false, showSeconds: false, zones: [] }),
    },
    {
        num: 17,
        id: "focus-timer",
        title: "Focus Timer",
        section: "productivity",
        sizes: [sz(2, 1, "compact"), sz(3, 2, "default"), sz(4, 3, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Focus countdown, break timer, stopwatch, and local completion log.",
        sample: "25:00 Focus · Ready",
        defaultConfig: () => ({}),
    },
    {
        num: 18,
        id: "quick-tasks",
        title: "Quick Tasks",
        section: "productivity",
        sizes: [sz(3, 2, "compact"), sz(4, 3, "default"), sz(6, 4, "expanded")],
        multi: true,
        network: false,
        privateAllowed: true,
        description: "Capture, reorder, and filter tasks across named lists.",
        sample: "☐ Review draft · ☑ Ship build",
        defaultConfig: () => ({ listFilter: "" }),
    },
    {
        num: 19,
        id: "scratchpad",
        title: "Scratchpad",
        section: "productivity",
        sizes: [sz(3, 2, "compact"), sz(4, 3, "default"), sz(8, 4, "expanded")],
        multi: true,
        network: false,
        privateAllowed: true,
        description: "Local plain-text notes with safe Markdown preview and .md export.",
        sample: "Quick notes · Auto-saved locally",
        defaultConfig: () => ({ noteId: "", preview: false }),
    },
    {
        num: 20,
        id: "calendar-countdown",
        title: "Calendar and Countdowns",
        section: "productivity",
        sizes: [sz(2, 2, "compact"), sz(4, 3, "default"), sz(6, 4, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Month calendar and civil-day countdowns to milestones.",
        sample: "Month grid · 12 days to milestone",
        defaultConfig: () => ({ weekStartsMonday: false }),
    },
    {
        num: 21,
        id: "habit-tracker",
        title: "Habit Tracker",
        section: "productivity",
        sizes: [sz(3, 2, "compact"), sz(4, 2, "default"), sz(6, 4, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Daily check-ins, streaks, and 28-day local calendar history.",
        sample: "Daily check-in · 5-day streak",
        defaultConfig: () => ({}),
    },

    // 5. Media and weather
    {
        num: 22,
        id: "now-playing",
        title: "Now Playing",
        section: "media",
        sizes: [sz(3, 1, "compact"), sz(4, 1, "default"), sz(6, 2, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Control active media playback scoped to this session’s tabs.",
        sample: "No media playing in this session",
        defaultConfig: () => ({}),
    },
    {
        num: 23,
        id: "weather",
        title: "Weather",
        section: "media",
        sizes: [sz(2, 1, "compact"), sz(3, 2, "default"), sz(6, 2, "expanded")],
        multi: true,
        network: true,
        privateAllowed: false,
        description: "Manual-coordinate forecast via Open-Meteo after explicit consent.",
        sample: "Opt-in coordinates · No geolocation",
        defaultConfig: () => ({ sourceId: "", unit: "C" }),
    },

    // 6. Developer tools and references
    {
        num: 24,
        id: "calculator",
        title: "Calculator and Units",
        section: "developer",
        sizes: [sz(2, 1, "compact"), sz(3, 2, "default"), sz(4, 3, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Arithmetic evaluator, unit conversions (temp, binary/decimal, rem), and keypad.",
        sample: "32 °F → 0 °C · 1 MiB → 1,048,576 B",
        defaultConfig: () => ({ remBase: 16, saveHistory: false }),
    },
    {
        num: 25,
        id: "color-contrast",
        title: "Color and Contrast",
        section: "developer",
        sizes: [sz(3, 2, "compact"), sz(4, 2, "default"), sz(6, 3, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Exact WCAG relative-luminance contrast checker for opaque sRGB colors.",
        sample: "#111111 on #FFFFFF · 18.88:1 AAA",
        defaultConfig: () => ({}),
    },
    {
        num: 26,
        id: "dev-codec",
        title: "Encoding and Hash Workbench",
        section: "developer",
        sizes: [sz(3, 2, "compact"), sz(4, 3, "default"), sz(6, 4, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Memory-only Base64, URL, JSON, JWT inspect, UUID, and SHA-256 tools.",
        sample: "Memory-only · Never persisted",
        defaultConfig: () => ({}),
    },
    {
        num: 27,
        id: "snippets",
        title: "Snippets",
        section: "developer",
        sizes: [sz(3, 2, "compact"), sz(4, 2, "default"), sz(6, 4, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Reusable text and command snippets with tag filter and one-click copy.",
        sample: "Filter by tag · Copy on click",
        defaultConfig: () => ({}),
    },
    {
        num: 28,
        id: "url-inspector",
        title: "URL Inspector",
        section: "developer",
        sizes: [sz(3, 2, "compact"), sz(4, 2, "default"), sz(6, 3, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Inspect and prune HTTP(S) query parameters offline with zero network calls.",
        sample: "Mask credentials · Edit query params",
        defaultConfig: () => ({}),
    },
    {
        num: 29,
        id: "shortcuts-guide",
        title: "Keyboard Shortcuts",
        section: "developer",
        sizes: [sz(3, 2, "compact"), sz(4, 2, "default"), sz(6, 3, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Live native browser bindings plus personal shortcut reminders.",
        sample: "Native menu shortcuts · Filterable",
        defaultConfig: () => ({}),
    },
    {
        num: 30,
        id: "pinned-references",
        title: "Pinned References and Browser Tools",
        section: "developer",
        sizes: [sz(2, 1, "compact"), sz(4, 2, "default"), sz(6, 3, "expanded")],
        multi: false,
        network: false,
        privateAllowed: true,
        description: "Quick browser tools and locally picked document references.",
        sample: "Settings · Privacy · Local docs",
        defaultConfig: () => ({}),
    },
];

export const WIDGET_MAP = new Map(WIDGET_CATALOG.map(w => [w.id, w]));

export function getWidgetDef(widgetType) {
    return WIDGET_MAP.get(widgetType) || null;
}

export function isWidgetAllowedInContext(def, ctx) {
    if (!def)
        return false;
    if (ctx.isEphemeral && !def.privateAllowed)
        return false;
    if (def.network && !ctx.allowsNetworkWidgets)
        return false;
    return true;
}

export const DASHBOARD_PRESETS = [
    {
        id: "focus",
        name: "Focus",
        description: "World Clock, Focus Timer, Quick Tasks, Scratchpad, Reading Queue, and Calendar.",
        items: [
            { widgetType: "world-clock", col: 0, row: 0, w: 3, h: 2 },
            { widgetType: "focus-timer", col: 3, row: 0, w: 3, h: 2 },
            { widgetType: "reading-queue", col: 6, row: 0, w: 6, h: 2 },
            { widgetType: "quick-tasks", col: 0, row: 2, w: 4, h: 3 },
            { widgetType: "scratchpad", col: 4, row: 2, w: 4, h: 3 },
            { widgetType: "calendar-countdown", col: 8, row: 2, w: 4, h: 3 },
        ],
    },
    {
        id: "developer",
        name: "Developer",
        description: "Open Tabs, Calculator, Color & Contrast, Encoding Workbench, Snippets, and URL Inspector.",
        items: [
            { widgetType: "open-tabs", col: 0, row: 0, w: 4, h: 2 },
            { widgetType: "calculator", col: 4, row: 0, w: 4, h: 2 },
            { widgetType: "color-contrast", col: 8, row: 0, w: 4, h: 2 },
            { widgetType: "dev-codec", col: 0, row: 2, w: 4, h: 3 },
            { widgetType: "snippets", col: 4, row: 2, w: 4, h: 3 },
            { widgetType: "url-inspector", col: 8, row: 2, w: 4, h: 3 },
        ],
    },
    {
        id: "privacy",
        name: "Privacy",
        description: "Privacy Summary, Site Permissions, Profile & Identity, and Browser Tools.",
        items: [
            { widgetType: "privacy-summary", col: 0, row: 0, w: 6, h: 2 },
            { widgetType: "profile-switcher", col: 6, row: 0, w: 3, h: 2 },
            { widgetType: "pinned-references", col: 9, row: 0, w: 3, h: 2 },
            { widgetType: "permissions-audit", col: 0, row: 2, w: 6, h: 3 },
        ],
    },
    {
        id: "empty",
        name: "Empty",
        description: "Remove all placed cards while keeping your saved notes, tasks, and collections.",
        items: [],
    },
];

const moduleCache = new Map();

export function loadWidgetModule(widgetType) {
    if (!WIDGET_MAP.has(widgetType))
        return Promise.reject(new Error(`Unknown widget type: ${widgetType}`));
    if (!moduleCache.has(widgetType)) {
        const url = `resource://ladybird/about-pages/dashboard/widgets/${widgetType}.js`;
        moduleCache.set(
            widgetType,
            import(url).catch(err => {
                moduleCache.delete(widgetType);
                throw err;
            })
        );
    }
    return moduleCache.get(widgetType);
}
