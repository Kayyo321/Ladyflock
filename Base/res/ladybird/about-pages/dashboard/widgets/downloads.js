/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #8 Downloads (downloads)
 * Scoped to caller's session/identity; exposes basename and progress only (omits local path).
 */

import { icon, ring } from "../icons.js";

function formatBytes(n) {
    if (!Number.isFinite(n) || n < 0)
        return "0 B";
    if (n < 1024)
        return `${n} B`;
    if (n < 1024 * 1024)
        return `${(n / 1024).toFixed(1)} KiB`;
    if (n < 1024 * 1024 * 1024)
        return `${(n / (1024 * 1024)).toFixed(1)} MiB`;
    return `${(n / (1024 * 1024 * 1024)).toFixed(2)} GiB`;
}

const STATUS_TEXT = {
    "in-progress": "Downloading",
    "paused": "Paused",
    "completed": "Completed",
    "canceled": "Canceled",
    "failed": "Failed",
};

const STATUS_GLYPH = {
    "in-progress": "downloads",
    "paused": "pause",
    "completed": "check",
    "canceled": "minus",
    "failed": "close",
};

function isActive(dl) {
    return dl.status === "in-progress" || dl.status === "paused";
}

function fraction(dl) {
    if (typeof dl.totalSize === "number" && dl.totalSize > 0)
        return Math.min(1, dl.downloadedSize / dl.totalSize);
    return null;
}

function sizeText(dl) {
    if (dl.status === "completed")
        return formatBytes(typeof dl.totalSize === "number" ? dl.totalSize : dl.downloadedSize);
    return typeof dl.totalSize === "number"
        ? `${formatBytes(dl.downloadedSize)} of ${formatBytes(dl.totalSize)}`
        : formatBytes(dl.downloadedSize);
}

export function mount(root, api) {
    let tier = api.tier;
    let state = { available: false, downloads: [] };
    let statusFilter = "all";

    root.classList.add("dl");

    // Expanded only: which downloads to list, and the way to the full page.
    const bar = document.createElement("div");
    bar.className = "dl-bar";
    const filterSeg = document.createElement("div");
    filterSeg.className = "dash-segments";
    filterSeg.setAttribute("role", "group");
    filterSeg.setAttribute("aria-label", "Filter downloads");
    const filterBtns = [];
    for (const [val, lbl] of [["all", "All"], ["active", "Active"], ["completed", "Completed"]]) {
        const b = document.createElement("button");
        b.type = "button";
        b.className = "dash-btn";
        b.textContent = lbl;
        b.addEventListener("click", () => {
            statusFilter = val;
            render();
        });
        filterBtns.push([val, b]);
        filterSeg.append(b);
    }
    const allBtn = document.createElement("button");
    allBtn.type = "button";
    allBtn.className = "dash-btn ghost small";
    allBtn.textContent = "All downloads";
    allBtn.addEventListener("click", () => api.openUrl("about:downloads", false));
    bar.append(filterSeg, allBtn);

    const heroEl = document.createElement("div");
    heroEl.className = "dl-hero";

    const listEl = document.createElement("ul");
    listEl.className = "dash-list dl-list";
    root.append(bar, heroEl, listEl);

    const act = async (dl, action) => {
        try {
            await api.request("downloads.action", { id: dl.id, action });
        } catch {
            await load();
        }
    };

    // Only the actions that are valid for the download's state.
    function actionButtons(dl) {
        const name = dl.fileName || "download";
        const out = [];
        const make = (glyph, verb, label, action) => {
            const b = document.createElement("button");
            b.type = "button";
            b.className = "dash-btn ghost icon small";
            b.setAttribute("aria-label", `${label} ${name}`);
            b.title = verb;
            b.append(icon(glyph, 14));
            b.addEventListener("click", () => act(dl, action));
            out.push(b);
        };
        if (dl.status === "in-progress" && dl.canResume)
            make("pause", "Pause", "Pause", "pause");
        else if (dl.status === "paused" && dl.canResume)
            make("play", "Resume", "Resume", "resume");
        if (isActive(dl))
            make("close", "Cancel", "Cancel", "cancel");
        else if (dl.status === "completed")
            make("external", "Show in folder", "Show in folder:", "reveal");
        return out;
    }

    function renderHero(dl) {
        heroEl.replaceChildren();
        heroEl.classList.toggle("is-active", isActive(dl));
        heroEl.dataset.status = dl.status;

        const frac = fraction(dl);
        const size = tier === "compact" ? 34 : tier === "expanded" ? 64 : 52;
        const dial = document.createElement("div");
        dial.className = "dl-dial";
        if (isActive(dl)) {
            const r = ring(size, tier === "compact" ? 3.5 : 5);
            r.set(frac ?? 0);
            dial.append(r.el);
            const inner = document.createElement("span");
            inner.className = "dl-dial-inner tabular";
            if (tier !== "compact" && frac !== null)
                inner.textContent = `${Math.floor(frac * 100)}%`;
            else
                inner.append(icon(STATUS_GLYPH[dl.status], tier === "compact" ? 14 : 18));
            dial.append(inner);
        } else {
            dial.classList.add("is-done");
            dial.append(icon(STATUS_GLYPH[dl.status] || "downloads", tier === "compact" ? 16 : 22));
        }

        const text = document.createElement("div");
        text.className = "dl-hero-text";
        // Long names shorten in the middle so the extension stays readable.
        const fileName = dl.fileName || "Download";
        const dot = fileName.lastIndexOf(".");
        const keepTail = dot > 0 && fileName.length - dot <= 8 ? fileName.length - dot + 4 : 0;
        const name = document.createElement("span");
        name.className = "dl-hero-name";
        name.title = dl.fileName || "";
        const stem = document.createElement("span");
        stem.className = "dash-truncate";
        stem.textContent = keepTail && fileName.length > 24 ? fileName.slice(0, -keepTail) : fileName;
        name.append(stem);
        if (keepTail && fileName.length > 24) {
            const tail = document.createElement("span");
            tail.className = "dl-hero-name-tail";
            tail.textContent = fileName.slice(-keepTail);
            name.append(tail);
        }
        const sub = document.createElement("span");
        sub.className = "dash-truncate dl-hero-sub tabular";
        const parts = [];
        if (tier === "compact" && dl.status === "in-progress" && frac !== null) {
            // The ring already says it is downloading; the line says how far.
            parts.push(`${Math.floor(frac * 100)}% of ${formatBytes(dl.totalSize)}`);
        } else {
            parts.push(STATUS_TEXT[dl.status] || dl.status);
            if (tier === "compact" && isActive(dl) && frac !== null)
                parts.push(`${Math.floor(frac * 100)}%`);
            else
                parts.push(sizeText(dl));
        }
        if (dl.status === "in-progress" && typeof dl.bytesPerSecond === "number" && dl.bytesPerSecond > 0 && tier !== "compact")
            parts.push(`${formatBytes(dl.bytesPerSecond)}/s`);
        sub.textContent = parts.join(" · ");
        text.append(name, sub);

        const actions = document.createElement("div");
        actions.className = "dl-hero-actions";
        actions.append(...actionButtons(dl));

        heroEl.append(dial, text, actions);
    }

    function render() {
        listEl.replaceChildren();
        heroEl.replaceChildren();
        if (!state.available) {
            bar.classList.add("hidden");
            heroEl.classList.add("hidden");
            api.setMeta("Unavailable");
            const empty = document.createElement("li");
            empty.className = "dash-empty-card";
            empty.textContent = "Downloads are not shown on ephemeral dashboards.";
            listEl.append(empty);
            return;
        }

        bar.classList.toggle("hidden", tier !== "expanded");
        for (const [val, b] of filterBtns)
            b.setAttribute("aria-pressed", String(val === statusFilter));
        const filtered = (state.downloads || []).filter(d => {
            if (statusFilter === "active")
                return d.status === "in-progress" || d.status === "paused";
            if (statusFilter === "completed")
                return d.status === "completed";
            return true;
        });
        // An empty list says so once, in its own words, without a count of nothing beside it.
        api.setMeta(filtered.length === 0 ? "" : filtered.length === 1 ? "1 file" : `${filtered.length} files`);

        if (filtered.length === 0) {
            heroEl.classList.add("hidden");
            const empty = document.createElement("li");
            empty.className = "dl-empty";
            empty.textContent = statusFilter === "all" ? "Nothing downloaded this session." : `No ${statusFilter} downloads.`;
            listEl.append(empty);
            return;
        }

        const cap = tier === "compact" ? 1 : tier === "expanded" ? 20 : 5;
        // The hero is the first active transfer anywhere in the list; the rest fill the cap.
        const hero = filtered.find(isActive) || filtered[0];
        const rest = filtered.filter(dl => dl !== hero).slice(0, cap - 1);
        heroEl.classList.remove("hidden");
        renderHero(hero);

        for (const dl of rest) {
            const li = document.createElement("li");
            li.className = "dash-list-item dl-item";
            li.dataset.status = dl.status;

            const mark = document.createElement("span");
            mark.className = "dl-mark";
            mark.append(icon(STATUS_GLYPH[dl.status] || "downloads", 12));

            const name = document.createElement("span");
            name.className = "dash-truncate dash-grow";
            name.textContent = dl.fileName || "Download";
            name.title = dl.fileName || "";

            const sub = document.createElement("span");
            sub.className = "dl-item-sub tabular";
            const frac = fraction(dl);
            sub.textContent = isActive(dl) && frac !== null
                ? `${Math.floor(frac * 100)}%`
                : dl.status === "completed"
                    ? formatBytes(typeof dl.totalSize === "number" ? dl.totalSize : dl.downloadedSize)
                    : STATUS_TEXT[dl.status] || dl.status;

            li.append(mark, name, sub);
            const buttons = actionButtons(dl);
            if (buttons.length) {
                const actions = document.createElement("span");
                actions.className = "dash-row-actions";
                actions.append(...buttons);
                li.append(actions);
            }
            listEl.append(li);
        }
    }

    async function load() {
        state = await api.request("downloads.list", {});
        render();
    }

    const offDl = api.onEvent("downloadsChanged", () => load());

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            offDl();
        },
    };
}
