/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #3 Open Tabs (open-tabs)
 * Shows caller's window, adds filtering, then groups same-session tabs across windows.
 * Uses opaque server-issued tab handles for focus/mute/close.
 */

import { icon, siteIcon } from "../icons.js";

function hostOf(urlStr) {
    try {
        return new URL(urlStr).hostname.replace(/^www\./, "");
    } catch {
        return "";
    }
}

export function mount(root, api) {
    let tier = api.tier;
    let windows = [];
    let filterText = "";

    root.classList.add("tabs");

    const filterRow = document.createElement("div");
    filterRow.className = "list-entry";
    const lens = icon("search", 13);
    lens.classList.add("list-entry-glyph");
    const filterInput = document.createElement("input");
    filterInput.type = "search";
    filterInput.className = "list-entry-input";
    filterInput.placeholder = "Filter open tabs…";
    filterInput.setAttribute("aria-label", "Filter open tabs");
    const refreshBtn = document.createElement("button");
    refreshBtn.type = "button";
    refreshBtn.className = "dash-btn ghost icon small";
    refreshBtn.setAttribute("aria-label", "Refresh tabs");
    refreshBtn.title = "Refresh";
    refreshBtn.append(icon("refresh", 13));
    refreshBtn.addEventListener("click", () => load());
    filterRow.append(lens, filterInput, refreshBtn);

    const listWrap = document.createElement("div");
    listWrap.className = "tabs-list";

    root.append(filterRow, listWrap);

    filterInput.addEventListener("input", () => {
        filterText = filterInput.value.trim().toLowerCase();
        render();
    });

    function render() {
        filterRow.classList.toggle("hidden", tier === "compact");
        listWrap.replaceChildren();

        const visibleWindows = tier === "expanded"
            ? windows
            : windows.filter(w => w.isCurrentWindow);
        const totalTabs = visibleWindows.reduce((s, w) => s + w.tabs.length, 0);
        api.setMeta(`${totalTabs} open`);

        let shown = 0;
        visibleWindows.forEach((win, winIdx) => {
            const matching = win.tabs.filter(t => {
                if (!filterText)
                    return true;
                return (t.title || "").toLowerCase().includes(filterText)
                    || (t.url || "").toLowerCase().includes(filterText);
            });
            if (!matching.length)
                return;

            if (tier === "expanded" && visibleWindows.length > 1) {
                const heading = document.createElement("div");
                heading.className = "dash-label tabs-heading";
                heading.textContent = win.isCurrentWindow ? "This window" : `Window ${winIdx + 1}`;
                listWrap.append(heading);
            }

            const ul = document.createElement("ul");
            ul.className = "dash-list";
            for (const tab of matching) {
                ++shown;
                const li = document.createElement("li");
                li.className = "dash-list-item";

                if (tab.isCurrent)
                    li.classList.add("is-current");

                const focusBtn = document.createElement("button");
                focusBtn.type = "button";
                focusBtn.className = "dash-link-btn";
                const mark = siteIcon({ className: "tabs-mark", url: tab.url, label: tab.title || hostOf(tab.url) || "N", favicon: tab.faviconBase64Png });
                const title = document.createElement("span");
                title.className = "tabs-title dash-truncate";
                title.textContent = tab.title || tab.url || "New Tab";
                focusBtn.append(mark, title);
                if (tab.isCurrent) {
                    const here = icon("check", 13);
                    here.classList.add("tabs-here");
                    here.setAttribute("role", "img");
                    here.setAttribute("aria-label", "Current tab");
                    focusBtn.append(here);
                }
                if (tab.isPinned) {
                    const pin = icon("pin", 12);
                    pin.classList.add("tabs-pinned");
                    pin.setAttribute("role", "img");
                    pin.setAttribute("aria-label", "Pinned");
                    focusBtn.append(pin);
                }
                focusBtn.addEventListener("click", async () => {
                    try {
                        await api.request("tabs.focus", { handle: tab.handle });
                    } catch {
                        await load();
                    }
                });
                li.append(focusBtn);

                if (tab.isAudible || tab.isMuted) {
                    const muteBtn = document.createElement("button");
                    muteBtn.type = "button";
                    muteBtn.className = "dash-btn ghost icon small";
                    const muteLabel = tab.isMuted ? "Unmute" : "Mute";
                    muteBtn.setAttribute("aria-label", `${muteLabel} ${tab.title || "tab"}`);
                    muteBtn.title = muteLabel;
                    muteBtn.append(icon(tab.isMuted ? "mute" : "sound", 14));
                    muteBtn.addEventListener("click", async () => {
                        try {
                            await api.request("tabs.toggleMute", { handle: tab.handle });
                        } finally {
                            await load();
                        }
                    });
                    li.append(muteBtn);
                }

                if (!tab.isSelf && tier !== "compact") {
                    const closeBtn = document.createElement("button");
                    closeBtn.type = "button";
                    closeBtn.className = "dash-btn ghost icon small tabs-close";
                    closeBtn.append(icon("close", 13));
                    closeBtn.setAttribute("aria-label", `Close ${tab.title || "tab"}`);
                    closeBtn.title = "Close tab";
                    closeBtn.addEventListener("click", async () => {
                        try {
                            await api.request("tabs.close", { handle: tab.handle });
                        } finally {
                            setTimeout(() => load(), 120);
                        }
                    });
                    li.append(closeBtn);
                }

                ul.append(li);
            }
            listWrap.append(ul);
        });

        if (shown === 0) {
            const empty = document.createElement("div");
            empty.className = "dash-empty-card";
            empty.textContent = filterText ? "No open tabs match that filter." : "No other tabs open.";
            listWrap.append(empty);
        }
    }

    async function load() {
        const res = await api.request("tabs.list", {});
        windows = res.windows || [];
        render();
    }

    const onVisibility = () => {
        if (!document.hidden)
            load();
    };
    document.addEventListener("visibilitychange", onVisibility);

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            document.removeEventListener("visibilitychange", onVisibility);
        },
    };
}
