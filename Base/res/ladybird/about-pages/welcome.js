/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

const form = document.querySelector("#welcome-form");
const setupOptions = document.querySelector("#setup-options");
const startBrowsing = document.querySelector("#start-browsing");
const searchEngines = document.querySelector("#search-engines");
const searchSuggestions = document.querySelector("#search-suggestions");
const searchSuggestionsEnabled = document.querySelector("#search-suggestions-enabled");
const searchSuggestionsDescription = document.querySelector("#search-suggestions-description");
const tabLayoutInputs = document.querySelectorAll('input[name="tab-layout"]');
const colorThemeInputs = document.querySelectorAll('input[name="color-theme"]');
const enableContentBlocking = document.querySelector("#enable-content-blocking");
const enableUpdateChecks = document.querySelector("#enable-update-checks");
const defaultBrowser = document.querySelector("#default-browser");
const defaultBrowserLabel = document.querySelector("#default-browser-label");
const defaultBrowserDescription = document.querySelector("#default-browser-description");
const makeDefaultBrowser = document.querySelector("#make-default-browser");
const saveStatus = document.querySelector("#save-status");

let settings;
let engines;
let features;
let initialized = false;
// Why the last attempt to make Ladybird the default browser failed, which is shown in place of the row's description.
let defaultBrowserFailure = null;

function setSearchEngineSettings(name, suggestions) {
    // NB: Keep our copy current, as the settings update for this change can arrive after the next one is made.
    settings.searchEngine = { ...settings.searchEngine, engine: name, suggestions };
    ladybird.sendMessage("setSearchEngineSettings", { engine: name, suggestions });
    updateSearchSuggestions();
}

function setSearchEngine(name) {
    setSearchEngineSettings(name, name !== null || settings.searchEngine.suggestions);
}

// The suggestions switch is only shown for a chosen engine that offers suggestions, and says where typed text goes.
function updateSearchSuggestions() {
    const name = settings.searchEngine.engine;
    const engine = engines.search.find(engine => engine.name === name);
    searchSuggestions.classList.toggle("hidden", !engine?.supportsSuggestions);
    searchSuggestionsEnabled.checked = settings.searchEngine.suggestions;
    if (engine)
        searchSuggestionsDescription.textContent = `Your typing in the address bar is sent to ${name} before you press Enter.`;
}

function renderSearchEngines() {
    const names = engines.search.map(engine => engine.name).sort((a, b) => a.localeCompare(b));

    searchEngines.replaceChildren();
    for (const name of names) {
        const tile = document.createElement("label");
        tile.className = "search-engine choice-card";

        const radio = document.createElement("input");
        radio.type = "radio";
        radio.name = "search-engine";
        radio.value = name;
        radio.checked = settings.searchEngine.engine === name;
        radio.addEventListener("change", () => {
            if (!initialized) return;
            setSearchEngine(name);
        });

        // A search engine is optional, so choosing the chosen one again goes without one.
        let wasChecked = false;
        tile.addEventListener("pointerdown", () => {
            wasChecked = radio.checked;
        });
        tile.addEventListener("click", event => {
            if (!initialized || !wasChecked || event.target !== radio) return;
            wasChecked = false;
            radio.checked = false;
            setSearchEngine(null);
        });

        // NB: Engines are shown by name only. Logos would mean shipping other companies' trademarks, or contacting
        //     their servers before anyone has chosen.
        const label = document.createElement("span");
        label.textContent = name;

        tile.append(radio, label);
        searchEngines.append(tile);
    }
}

function tabLayout() {
    if (!settings.tabs.verticalTabsEnabled) return "horizontal";
    return settings.tabs.everythingInSidebar ? "sidebar" : "vertical";
}

function updateTabLayout() {
    const layout = tabLayout();
    for (const input of tabLayoutInputs) input.checked = input.value === layout;
}

const COLOR_THEME_BASES = {
    harbor: "#2f6fd6",
    glacier: "#1798b8",
    lagoon: "#12957f",
    moss: "#3b8f3e",
    saffron: "#c99500",
    copper: "#d0631c",
    cherry: "#d1344b",
    blossom: "#d2459a",
    lavender: "#7563d4",
};

const NEUTRAL_MARK_PALETTE = {
    light: { light: "#c674f8", back: "#9c50da", mid: "#742dc5", deep: "#4d0b9e", bird: "#4e1599", solid: "#4b22ad" },
    dark: { light: "#e2b4ff", back: "#c68cff", mid: "#a567f5", deep: "#7d43dc", bird: "#d3b8ff", solid: "#b496ff" },
};

function hexToRgb(hex) {
    const clean = hex.replace("#", "");
    return [
        parseInt(clean.slice(0, 2), 16),
        parseInt(clean.slice(2, 4), 16),
        parseInt(clean.slice(4, 6), 16),
    ];
}

function rgbToHex([r, g, b]) {
    return "#" + [r, g, b].map(v => Math.max(0, Math.min(255, Math.round(v))).toString(16).padStart(2, "0")).join("");
}

function rgbToHsl([r, g, b]) {
    r /= 255;
    g /= 255;
    b /= 255;
    const max = Math.max(r, g, b);
    const min = Math.min(r, g, b);
    const l = (max + min) / 2;
    let h = 0;
    let s = 0;
    if (max !== min) {
        const d = max - min;
        s = l > 0.5 ? d / (2 - max - min) : d / (max + min);
        if (max === r) h = (g - b) / d + (g < b ? 6 : 0);
        else if (max === g) h = (b - r) / d + 2;
        else h = (r - g) / d + 4;
        h *= 60;
    }
    return [h, s, l];
}

function hslToRgb(hDeg, s, l) {
    const h = (((hDeg % 360) + 360) % 360) / 360;
    const sat = Math.max(0, Math.min(1, s));
    const lit = Math.max(0, Math.min(1, l));
    if (sat === 0) {
        const v = Math.round(lit * 255);
        return [v, v, v];
    }
    const q = lit < 0.5 ? lit * (1 + sat) : lit + sat - lit * sat;
    const p = 2 * lit - q;
    const hue2rgb = t => {
        if (t < 0) t += 1;
        if (t > 1) t -= 1;
        if (t < 1 / 6) return p + (q - p) * 6 * t;
        if (t < 1 / 2) return q;
        if (t < 2 / 3) return p + (q - p) * (2 / 3 - t) * 6;
        return p;
    };
    return [Math.round(hue2rgb(h + 1 / 3) * 255), Math.round(hue2rgb(h) * 255), Math.round(hue2rgb(h - 1 / 3) * 255)];
}

function relativeLuminance([r, g, b]) {
    const lin = c => {
        const v = c / 255;
        return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
    };
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

function contrastRatio(a, b) {
    const la = relativeLuminance(a);
    const lb = relativeLuminance(b);
    return (Math.max(la, lb) + 0.05) / (Math.min(la, lb) + 0.05);
}

function withContrast(hDeg, s, l, bgRgb, minRatio, lighten) {
    const step = lighten ? 0.02 : -0.02;
    let rgb = hslToRgb(hDeg, s, l);
    while (contrastRatio(rgb, bgRgb) < minRatio) {
        l += step;
        if (l <= 0.02 || l >= 0.98) break;
        rgb = hslToRgb(hDeg, s, l);
    }
    return rgbToHex(rgb);
}

function deriveMarkPalette(themeName, dark) {
    const baseHex = COLOR_THEME_BASES[themeName];
    if (!baseHex) return dark ? NEUTRAL_MARK_PALETTE.dark : NEUTRAL_MARK_PALETTE.light;

    const [h, s] = rgbToHsl(hexToRgb(baseHex));
    const satHi = Math.min(0.92, Math.max(0.68, s * 1.05));
    const satMid = Math.min(0.86, Math.max(0.62, s));
    const satDeep = Math.min(0.84, Math.max(0.58, s * 0.95));

    if (!dark) {
        const bg = [255, 255, 255];
        return {
            light: withContrast(h - 6, satHi, 0.64, bg, 2.5, false),
            back: withContrast(h - 3, satMid, 0.54, bg, 3.4, false),
            mid: withContrast(h, satMid, 0.44, bg, 4.6, false),
            deep: withContrast(h + 4, satDeep, 0.32, bg, 6.8, false),
            bird: withContrast(h + 4, satDeep, 0.3, bg, 7.2, false),
            solid: withContrast(h, satDeep, 0.34, bg, 7.0, false),
        };
    }

    const bg = [28, 28, 30];
    return {
        light: withContrast(h - 6, Math.min(0.95, satHi), 0.82, bg, 9.5, true),
        back: withContrast(h - 3, satHi, 0.72, bg, 7.2, true),
        mid: withContrast(h, satMid, 0.62, bg, 5.2, true),
        deep: withContrast(h + 4, satMid, 0.5, bg, 3.6, true),
        bird: withContrast(h - 4, Math.min(0.92, satHi), 0.8, bg, 8.8, true),
        solid: withContrast(h, satHi, 0.72, bg, 7.5, true),
    };
}

function applyMarkTheme(colorTheme) {
    const root = document.documentElement.style;
    for (const [scheme, dark] of [
        ["light", false],
        ["dark", true],
    ]) {
        const palette = deriveMarkPalette(colorTheme, dark);
        for (const key of ["light", "back", "mid", "deep", "bird", "solid"]) {
            root.setProperty(`--mark-${scheme}-${key}`, palette[key]);
        }
    }
}

function updateColorTheme() {
    const colorTheme = settings.appearance.colorTheme || "lavender";
    for (const input of colorThemeInputs) input.checked = input.value === colorTheme;
    applyMarkTheme(colorTheme);
}

// The row is only shown where the system can tell which browser is the default, and lets Ladybird become it.
function updateDefaultBrowser() {
    const isDefault = settings.defaultBrowser === "default";
    const canMakeDefault = settings.defaultBrowser === "not-default";

    defaultBrowser.classList.toggle("hidden", !isDefault && !canMakeDefault);
    defaultBrowserDescription.textContent =
        defaultBrowserFailure ??
        (isDefault ? "Links from other apps open in Ladyflock." : "Open links from other apps in Ladyflock.");
    makeDefaultBrowser.classList.toggle("hidden", !canMakeDefault);

    // A control row is filled under the pointer, and a click anywhere on it reaches the control its label names. This
    // row is one only while there is something for the button to do.
    defaultBrowser.classList.toggle("control-row", canMakeDefault);
    if (canMakeDefault) defaultBrowserLabel.htmlFor = makeDefaultBrowser.id;
    else defaultBrowserLabel.removeAttribute("for");
}

function updateUpdateChecks() {
    const networkFeatures = settings.backgroundNetworking.features;
    if (Object.hasOwn(networkFeatures, "updateChecks")) enableUpdateChecks.checked = networkFeatures.updateChecks;
    else enableUpdateChecks.closest(".switch-row").remove();
}

function initialize() {
    if (initialized || !settings || !engines || !features) return;
    initialized = true;

    document.querySelector("#tab-layout-section").classList.toggle("hidden", !features.verticalTabs);
    updateTabLayout();
    updateColorTheme();
    updateDefaultBrowser();

    renderSearchEngines();
    updateSearchSuggestions();

    enableContentBlocking.checked = settings.contentBlockers.enabled;
    updateUpdateChecks();
    setupOptions.disabled = false;
    startBrowsing.disabled = false;
}

for (const input of tabLayoutInputs) {
    input.addEventListener("change", () => {
        if (!initialized || !input.checked || !features.verticalTabs) return;
        // NB: Keep our copy current, as the settings update for this change can arrive after the next one is made.
        settings.tabs = {
            ...settings.tabs,
            verticalTabsEnabled: input.value !== "horizontal",
            everythingInSidebar: input.value === "sidebar",
        };
        ladybird.sendMessage("setTabSettings", settings.tabs);
    });
}

// Picking a color recolors the window around the page and the emblem at once.
for (const input of colorThemeInputs) {
    input.addEventListener("change", () => {
        if (!initialized || !input.checked) return;
        // NB: Keep our copy current, as the settings update for this change can arrive after the next one is made.
        settings.appearance = { ...settings.appearance, colorTheme: input.value };
        applyMarkTheme(input.value);
        ladybird.sendMessage("setAppearance", settings.appearance);
    });
}

searchSuggestionsEnabled.addEventListener("change", () => {
    if (!initialized) return;
    setSearchEngineSettings(settings.searchEngine.engine, searchSuggestionsEnabled.checked);
});

enableContentBlocking.addEventListener("change", () => {
    if (!initialized) return;

    // Enable the lists before the content blocker, which then downloads every enabled list.
    if (enableContentBlocking.checked) {
        for (const identifier of ["easyList", "easyPrivacy"])
            ladybird.sendMessage("setContentBlockerListEnabled", { identifier, enabled: true });
    }

    ladybird.sendMessage("setContentBlockerEnabled", enableContentBlocking.checked);
    ladybird.sendMessage("setFilterListUpdatesEnabled", enableContentBlocking.checked);
});

enableUpdateChecks.addEventListener("change", () => {
    if (!initialized) return;
    ladybird.sendMessage("setUpdateChecksEnabled", enableUpdateChecks.checked);
});

// The system may ask before it changes the default browser, so the button waits for its answer.
makeDefaultBrowser.addEventListener("click", () => {
    if (!initialized) return;

    makeDefaultBrowser.disabled = true;
    defaultBrowserFailure = null;
    updateDefaultBrowser();

    ladybird.sendMessage("makeDefaultBrowser");
});

form.addEventListener("submit", event => {
    event.preventDefault();
    if (!initialized || startBrowsing.disabled) return;

    setupOptions.disabled = true;
    startBrowsing.disabled = true;
    saveStatus.textContent = "";
    ladybird.sendMessage("completeFirstRun");
});

document.addEventListener("WebUILoaded", () => {
    ladybird.sendMessage("loadFeatures");
    ladybird.sendMessage("loadAvailableEngines");
    ladybird.sendMessage("loadCurrentSettings");
});

document.addEventListener("WebUIMessage", event => {
    const { name, data } = event.detail;

    if (name === "loadSettings") {
        settings = { ...settings, ...data };

        if (initialized) {
            if (Object.hasOwn(data, "tabs")) updateTabLayout();
            if (Object.hasOwn(data, "appearance")) updateColorTheme();
            if (Object.hasOwn(data, "searchEngine")) {
                for (const radio of searchEngines.querySelectorAll("input"))
                    radio.checked = radio.value === settings.searchEngine.engine;
                updateSearchSuggestions();
            }
            if (Object.hasOwn(data, "contentBlockers"))
                enableContentBlocking.checked = settings.contentBlockers.enabled;
            if (Object.hasOwn(data, "backgroundNetworking")) updateUpdateChecks();
            if (Object.hasOwn(data, "defaultBrowser")) {
                defaultBrowserFailure = null;
                updateDefaultBrowser();
            }
        }
    } else if (name === "loadEngines") {
        engines = data;
    } else if (name === "loadFeatures") {
        features = data;
    } else if (name === "defaultBrowserResult") {
        makeDefaultBrowser.disabled = false;
        defaultBrowserFailure = data;
        updateDefaultBrowser();
    } else if (name === "firstRunCompleted") {
        location.replace(data);
    } else if (name === "firstRunError") {
        saveStatus.textContent = data;
        setupOptions.disabled = false;
        startBrowsing.disabled = false;
    }

    initialize();
});
