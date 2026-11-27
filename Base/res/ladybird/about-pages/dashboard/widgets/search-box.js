/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #2 Search Box (search-box)
 * Uses browser's configured search engine via format_search_query_for_navigation.
 * No suggestions and no autofocus.
 */

import { icon } from "../icons.js";

export function mount(root, api) {
    let tier = api.tier;
    let engineState = { defaultEngine: null, engines: [] };
    let selectedEngine = "";

    root.classList.add("search");

    const row = document.createElement("div");
    row.className = "search-pill";

    const lens = icon("search", 16);
    lens.classList.add("search-lens");

    const input = document.createElement("input");
    input.type = "search";
    input.className = "search-input";
    input.autocomplete = "off";
    input.placeholder = "Search the web…";
    input.setAttribute("aria-label", "Web search");

    const submitBtn = document.createElement("button");
    submitBtn.type = "button";
    submitBtn.className = "search-go hidden";
    submitBtn.setAttribute("aria-label", "Search");
    submitBtn.title = "Search";
    submitBtn.append(icon("arrow-right", 14));

    row.append(lens, input, submitBtn);

    const engineBar = document.createElement("div");
    engineBar.className = "dash-row-between search-engines";

    const engineSelect = document.createElement("select");
    engineSelect.className = "search-engine-select";
    engineSelect.setAttribute("aria-label", "Search engine for this query");

    const settingsBtn = document.createElement("button");
    settingsBtn.type = "button";
    settingsBtn.className = "dash-btn ghost small";
    settingsBtn.textContent = "Search settings";
    settingsBtn.addEventListener("click", () => api.openUrl("about:settings#search", false));

    const emptyNotice = document.createElement("div");
    emptyNotice.className = "dash-notice hidden";
    const noticeText = document.createElement("span");
    noticeText.textContent = "No default search engine configured.";
    const openSettingsBtn = document.createElement("button");
    openSettingsBtn.type = "button";
    openSettingsBtn.className = "dash-btn small";
    openSettingsBtn.textContent = "Search settings";
    openSettingsBtn.addEventListener("click", () => api.openUrl("about:settings#search", false));
    emptyNotice.append(noticeText, openSettingsBtn);

    engineBar.append(engineSelect, settingsBtn);
    root.append(row, engineBar, emptyNotice);

    input.addEventListener("input", () => submitBtn.classList.toggle("hidden", input.value.trim() === ""));

    function render() {
        const hasEngine = Boolean(engineState.defaultEngine || engineState.engines.length > 0);
        row.classList.toggle("hidden", !hasEngine);
        emptyNotice.classList.toggle("hidden", hasEngine);
        engineBar.classList.toggle("hidden", !hasEngine || tier !== "expanded");

        const activeName = selectedEngine || engineState.defaultEngine || "";
        api.setMeta(tier === "compact" ? "" : activeName);
        input.placeholder = activeName ? `Search with ${activeName}…` : "Search the web…";

        engineSelect.replaceChildren();
        for (const e of engineState.engines) {
            const opt = document.createElement("option");
            opt.value = e.name;
            opt.textContent = e.name;
            engineSelect.append(opt);
        }
        if (activeName)
            engineSelect.value = activeName;
    }

    engineSelect.addEventListener("change", () => {
        selectedEngine = engineSelect.value;
        render();
    });

    async function doSearch() {
        const q = input.value.trim();
        if (!q)
            return;
        try {
            await api.request("search.submit", {
                query: q,
                engineName: selectedEngine || engineState.defaultEngine || "",
            });
        } catch (err) {
            api.announce(err.message || "Search failed.");
        }
    }

    submitBtn.addEventListener("click", doSearch);
    input.addEventListener("keydown", e => {
        if (e.key === "Enter") {
            e.preventDefault();
            doSearch();
        }
    });

    api.request("search.engines", {})
        .then(res => {
            engineState = res;
            selectedEngine = res.defaultEngine || "";
            render();
        })
        .catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {},
    };
}
