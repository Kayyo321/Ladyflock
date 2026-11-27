/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #15 Browser Health (browser-health)
 * Shows eligible tab count, available per-tab CPU/memory samples (marking shared-process tabs;
 * never summing per-tab shares as browser RAM), and native discard for eligible background tabs.
 */

import { icon, siteIcon } from "../icons.js";

function fmtMiB(bytes) {
    if (typeof bytes !== "number" || !Number.isFinite(bytes))
        return null;
    return `${(bytes / (1024 * 1024)).toFixed(1)} MiB`;
}

function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className)
        node.className = className;
    if (text !== undefined)
        node.textContent = text;
    return node;
}

export function mount(root, api) {
    let tier = api.tier;
    let snapshot = { eligibleTabCount: 0, loadedTabCount: 0, tabs: [] };
    let pollTimer = null;
    root.classList.add("health");

    const headRow = el("div", "health-head");
    const figuresEl = el("dl", "health-figures");
    const refreshBtn = el("button", "dash-btn ghost icon small health-refresh");
    refreshBtn.type = "button";
    refreshBtn.setAttribute("aria-label", "Refresh samples");
    refreshBtn.title = "Refresh";
    refreshBtn.append(icon("refresh", 14));
    refreshBtn.addEventListener("click", () => load());
    headRow.append(figuresEl, refreshBtn);

    const listEl = el("ul", "dash-list health-list");
    root.append(headRow, listEl);

    function figure(value, label) {
        const cell = el("div", "health-figure");
        cell.append(el("dd", "health-figure-value tabular", String(value)), el("dt", "dash-muted", label));
        return cell;
    }

    function render() {
        figuresEl.replaceChildren();
        figuresEl.append(figure(snapshot.eligibleTabCount, "tabs"), figure(snapshot.loadedTabCount, "loaded"));
        api.setMeta(`${snapshot.loadedTabCount} active`);
        listEl.replaceChildren();

        if (tier === "compact")
            return;

        const sorted = [...(snapshot.tabs || [])].sort((a, b) => (b.memoryBytes ?? -1) - (a.memoryBytes ?? -1));
        const sampled = sorted.filter(t => typeof t.cpuPercent === "number" || fmtMiB(t.memoryBytes));
        if (sorted.length === 0) {
            listEl.append(el("li", "health-note", "No tabs to sample."));
            return;
        }
        if (sampled.length === 0)
            listEl.append(el("li", "health-note", "Samples are unavailable right now."));

        const cap = tier === "expanded" ? 12 : 3;
        for (const t of sorted.slice(0, cap)) {
            const li = el("li", "dash-list-item health-row");

            const title = el("span", "health-title dash-truncate", t.title || t.url || "Tab");

            const parts = [];
            if (typeof t.cpuPercent === "number")
                parts.push(`${t.cpuPercent.toFixed(1)}% CPU`);
            const mem = fmtMiB(t.memoryBytes);
            if (mem)
                parts.push(t.sharedWithOtherTabs > 0 ? `${mem} (shared with ${t.sharedWithOtherTabs})` : mem);
            if (!t.isLoaded)
                parts.push("Discarded");

            const meta = el("span", "health-meta dash-muted tabular dash-truncate", parts.length ? parts.join(" · ") : "No sample");
            li.append(siteIcon({ className: "dash-site-icon", url: t.url, label: t.title, favicon: t.faviconBase64Png }), title, meta);

            if (t.canDiscard && t.handle) {
                const actions = el("span", "dash-row-actions");
                const discBtn = el("button", "dash-btn ghost small");
                discBtn.type = "button";
                discBtn.textContent = "Discard";
                discBtn.setAttribute("aria-label", `Discard ${t.title || t.url || "tab"}`);
                discBtn.addEventListener("click", async () => {
                    try {
                        await api.request("health.discardTab", { handle: t.handle });
                    } finally {
                        setTimeout(() => load(), 150);
                    }
                });
                actions.append(discBtn);
                li.append(actions);
            }
            listEl.append(li);
        }
    }

    async function load() {
        snapshot = await api.request("health.snapshot", {});
        render();
    }

    function schedulePoll() {
        clearInterval(pollTimer);
        if (!document.hidden) {
            pollTimer = setInterval(() => {
                if (!document.hidden)
                    load().catch(() => {});
            }, 5000);
        }
    }

    const onVis = () => {
        if (document.hidden)
            clearInterval(pollTimer);
        else {
            load().catch(() => {});
            schedulePoll();
        }
    };
    document.addEventListener("visibilitychange", onVis);

    load().catch(err => api.reportError(err.message));
    schedulePoll();

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            clearInterval(pollTimer);
            document.removeEventListener("visibilitychange", onVis);
        },
    };
}
