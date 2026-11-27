/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #7 Recent History (recent-history)
 * Scoped bounded history query with filter, pagination, and single-entry removal.
 */

import { icon, siteIcon } from "../icons.js";

function dayKey(ms) {
    const d = new Date(ms);
    return `${d.getFullYear()}-${d.getMonth()}-${d.getDate()}`;
}

function dayLabel(ms) {
    const d = new Date(ms);
    const today = new Date();
    const yesterday = new Date(today.getFullYear(), today.getMonth(), today.getDate() - 1);
    if (dayKey(ms) === dayKey(today.getTime()))
        return "Today";
    if (dayKey(ms) === dayKey(yesterday.getTime()))
        return "Yesterday";
    const sameYear = d.getFullYear() === today.getFullYear();
    return d.toLocaleDateString([], sameYear
        ? { weekday: "long", month: "short", day: "numeric" }
        : { year: "numeric", month: "short", day: "numeric" });
}

function hostOf(url) {
    try {
        return new URL(url).hostname.replace(/^www\./, "");
    } catch {
        return "";
    }
}

export function mount(root, api) {
    let tier = api.tier;
    let entries = [];
    let hasMore = false;
    let available = false;
    let query = "";
    let offset = 0;

    root.classList.add("hist");

    // The filter reads as a quiet line above the list, not a boxed field.
    const bar = document.createElement("label");
    bar.className = "hist-filter";
    bar.append(icon("search", 14));
    const searchInput = document.createElement("input");
    searchInput.type = "search";
    searchInput.className = "hist-filter-input";
    searchInput.placeholder = "Filter history…";
    searchInput.setAttribute("aria-label", "Filter recent history");
    let debounceTimer = null;
    searchInput.addEventListener("input", () => {
        clearTimeout(debounceTimer);
        debounceTimer = setTimeout(() => {
            query = searchInput.value.trim();
            offset = 0;
            load();
        }, 200);
    });
    bar.append(searchInput);

    const listEl = document.createElement("ul");
    listEl.className = "dash-list hist-list";

    const pager = document.createElement("div");
    pager.className = "hist-footer hidden";
    const moreBtn = document.createElement("button");
    moreBtn.type = "button";
    moreBtn.className = "dash-btn ghost small";
    moreBtn.textContent = "Load more";
    moreBtn.addEventListener("click", () => load(true));
    const allBtn = document.createElement("button");
    allBtn.type = "button";
    allBtn.className = "dash-btn ghost small";
    allBtn.textContent = "All history";
    allBtn.addEventListener("click", () => api.openUrl("about:history", false));
    pager.append(moreBtn, allBtn);

    root.append(bar, listEl, pager);

    function favicon(e) {
        return siteIcon({ className: "hist-favicon", url: e.url, label: hostOf(e.url) || e.title, favicon: e.faviconBase64Png });
    }

    function render() {
        listEl.replaceChildren();
        if (!available) {
            bar.classList.add("hidden");
            pager.classList.add("hidden");
            api.setMeta("Unavailable");
            const msg = document.createElement("li");
            msg.className = "dash-empty-card";
            msg.textContent = "History is disabled in this session.";
            listEl.append(msg);
            return;
        }

        bar.classList.toggle("hidden", tier === "compact");
        pager.classList.toggle("hidden", tier !== "expanded");
        moreBtn.classList.toggle("hidden", !hasMore);
        api.setMeta(`${entries.length} shown`);

        if (entries.length === 0) {
            const msg = document.createElement("li");
            msg.className = "hist-empty";
            msg.textContent = query ? "No history entries match that filter." : "No recent history yet.";
            listEl.append(msg);
            return;
        }

        const cap = tier === "compact" ? 5 : entries.length;
        let lastDay = null;
        for (const e of entries.slice(0, cap)) {
            const when = Number(e.lastVisitedTime) || 0;
            const key = when > 0 ? dayKey(when) : "";
            if (key !== lastDay) {
                lastDay = key;
                const head = document.createElement("li");
                head.className = "hist-day";
                head.textContent = when > 0 ? dayLabel(when) : "Earlier";
                listEl.append(head);
            }

            const li = document.createElement("li");
            li.className = "dash-list-item hist-item";

            const btn = document.createElement("button");
            btn.type = "button";
            btn.className = "dash-link-btn";
            btn.title = e.url;
            const title = document.createElement("span");
            title.className = "dash-truncate hist-title";
            title.textContent = e.title || e.url;
            btn.append(favicon(e), title);
            if (tier === "expanded") {
                const host = hostOf(e.url);
                if (host && host !== (e.title || e.url)) {
                    const hostEl = document.createElement("span");
                    hostEl.className = "dash-truncate hist-host";
                    hostEl.textContent = host;
                    btn.append(hostEl);
                }
            }
            btn.addEventListener("click", () => api.openUrl(e.url, false));
            li.append(btn);

            if (when > 0 && tier !== "compact") {
                const time = document.createElement("span");
                time.className = "hist-time tabular";
                time.textContent = new Date(when).toLocaleTimeString([], { hour: "numeric", minute: "2-digit" });
                li.append(time);
            }

            if (tier !== "compact") {
                const actions = document.createElement("span");
                actions.className = "dash-row-actions";
                const label = `Remove ${e.title || e.url} from history`;
                const rmBtn = document.createElement("button");
                rmBtn.type = "button";
                rmBtn.className = "dash-btn ghost icon small";
                rmBtn.setAttribute("aria-label", label);
                rmBtn.title = "Remove from history";
                rmBtn.append(icon("close", 14));
                rmBtn.addEventListener("click", async () => {
                    await api.request("history.remove", { url: e.url });
                    entries = entries.filter(x => x.url !== e.url);
                    render();
                });
                actions.append(rmBtn);
                li.append(actions);
            }

            listEl.append(li);
        }
    }

    async function load(append = false) {
        const limit = tier === "expanded" ? 25 : 12;
        const res = await api.request("history.list", {
            query,
            offset: append ? offset : 0,
            limit,
        });
        available = Boolean(res.available);
        hasMore = Boolean(res.hasMore);
        if (append) {
            entries = [...entries, ...(res.entries || [])];
        } else {
            entries = res.entries || [];
        }
        offset = entries.length;
        render();
    }

    const offCleared = api.onEvent("historyCleared", () => {
        offset = 0;
        load(false);
    });

    load(false).catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            clearTimeout(debounceTimer);
            offCleared();
        },
    };
}
