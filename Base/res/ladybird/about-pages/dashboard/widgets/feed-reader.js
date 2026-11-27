/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #10 Feed Reader (feed-reader)
 * Explicit opt-in per-source HTTPS consent; zero requests until confirmed.
 * Manual refresh default; optional 30-min refresh while visible.
 * Preserves last successful items across failed/offline refreshes; plain-text excerpts only.
 */

import { icon } from "../icons.js";

// A short age when the feed's date parses; otherwise the feed's own text.
function ageText(raw) {
    if (!raw)
        return "";
    const t = Date.parse(raw);
    // Only trust a parse that lands in a plausible range; anything else is shown as written.
    if (!Number.isFinite(t) || t > Date.now() + 86400000 || t < Date.UTC(1995, 0, 1))
        return raw;
    const mins = Math.round((Date.now() - t) / 60000);
    if (mins < 1)
        return "now";
    if (mins < 60)
        return `${mins}m`;
    if (mins < 48 * 60)
        return `${Math.round(mins / 60)}h`;
    if (mins < 14 * 24 * 60)
        return `${Math.round(mins / 1440)}d`;
    return new Date(t).toLocaleDateString([], { month: "short", day: "numeric" });
}

export function mount(root, api) {
    let tier = api.tier;
    let state = { allowed: false, offline: false, grants: [] };
    let selectedSourceId = "all";
    let unreadOnly = false;
    let readUrls = new Set();
    let statusMsg = "";
    let autoTimer = null;

    root.classList.add("feed");

    const topBar = document.createElement("div");
    topBar.className = "feed-bar";

    const sourceSel = document.createElement("select");
    sourceSel.className = "dash-select feed-source";
    sourceSel.setAttribute("aria-label", "Filter feed source");
    sourceSel.addEventListener("change", () => {
        selectedSourceId = sourceSel.value;
        render();
    });

    const btnGroup = document.createElement("div");
    btnGroup.className = "feed-bar-actions";
    const unreadBtn = document.createElement("button");
    unreadBtn.type = "button";
    unreadBtn.className = "dash-btn ghost small feed-unread-toggle";
    unreadBtn.textContent = "Unread";
    unreadBtn.addEventListener("click", () => {
        unreadOnly = !unreadOnly;
        render();
    });
    const refreshBtn = document.createElement("button");
    refreshBtn.type = "button";
    refreshBtn.className = "dash-btn ghost icon small";
    refreshBtn.setAttribute("aria-label", "Refresh");
    refreshBtn.title = "Refresh";
    refreshBtn.append(icon("refresh", 15));
    refreshBtn.addEventListener("click", () => refreshAll());
    btnGroup.append(unreadBtn, refreshBtn);
    topBar.append(sourceSel, btnGroup);

    const statusLine = document.createElement("div");
    statusLine.className = "feed-status hidden";

    const setupBox = document.createElement("div");
    setupBox.className = "feed-setup hidden";

    const listEl = document.createElement("ul");
    listEl.className = "dash-list feed-list";

    root.append(topBar, statusLine, setupBox, listEl);

    function feedGrants() {
        return (state.grants || []).filter(g => g.kind === "feed");
    }

    // The consent form. It is drawn in the tile and in the Settings dialog, so it is styled
    // by its own classes and does not depend on the tile around it.
    function renderSetup(container) {
        container.replaceChildren();
        container.classList.add("net-consent");

        const explain = document.createElement("p");
        explain.className = "net-consent-text";
        const glyph = document.createElement("span");
        glyph.className = "net-consent-glyph";
        glyph.append(icon("feed-reader", 16));
        const words = document.createElement("span");
        words.textContent = "Adding a feed makes HTTPS requests to that destination (exposing your connection’s egress IP, without cookies or referrer). Manual refresh is the default.";
        explain.append(glyph, words);

        const urlInp = document.createElement("input");
        urlInp.type = "url";
        urlInp.className = "dash-input net-consent-field";
        urlInp.placeholder = "https://example.org/feed.xml";
        urlInp.setAttribute("aria-label", "HTTPS feed URL");

        const autoLbl = document.createElement("label");
        autoLbl.className = "net-consent-check";
        const autoCb = document.createElement("input");
        autoCb.type = "checkbox";
        const autoTxt = document.createElement("span");
        autoTxt.textContent = "Allow automatic refresh while visible (every 30 min)";
        autoLbl.append(autoCb, autoTxt);

        const errEl = document.createElement("span");
        errEl.className = "form-error net-consent-error hidden";

        const confirmBtn = document.createElement("button");
        confirmBtn.type = "button";
        confirmBtn.className = "dash-btn primary net-consent-confirm";
        confirmBtn.textContent = "Confirm HTTPS feed";
        confirmBtn.addEventListener("click", async () => {
            errEl.classList.add("hidden");
            const url = urlInp.value.trim();
            if (!url)
                return;
            try {
                const res = await api.request("network.grant", {
                    kind: "feed",
                    url,
                    autoRefresh: autoCb.checked,
                });
                urlInp.value = "";
                await load();
                // Trigger initial user-consented fetch
                await refreshOne(res.sourceId);
            } catch (err) {
                errEl.textContent = err.message || "Invalid feed URL.";
                errEl.classList.remove("hidden");
            }
        });

        container.append(explain, urlInp, autoLbl, errEl, confirmBtn);
    }

    function render() {
        const grants = feedGrants();
        listEl.replaceChildren();

        if (!state.allowed) {
            topBar.classList.add("hidden");
            setupBox.classList.add("hidden");
            statusLine.classList.add("hidden");
            api.setMeta("Disabled");
            const empty = document.createElement("li");
            empty.className = "dash-empty-card";
            empty.textContent = "Feed Reader is disabled in private or temporary sessions.";
            listEl.append(empty);
            return;
        }

        if (grants.length === 0) {
            topBar.classList.add("hidden");
            statusLine.classList.add("hidden");
            setupBox.classList.remove("hidden");
            api.setMeta("Not configured");
            renderSetup(setupBox);
            return;
        }

        setupBox.classList.add("hidden");
        topBar.classList.remove("hidden");
        unreadBtn.setAttribute("aria-pressed", String(unreadOnly));
        unreadBtn.classList.toggle("hidden", tier === "compact");

        sourceSel.replaceChildren();
        const allOpt = document.createElement("option");
        allOpt.value = "all";
        allOpt.textContent = `All feeds (${grants.length})`;
        sourceSel.append(allOpt);
        for (const g of grants) {
            const opt = document.createElement("option");
            opt.value = g.sourceId;
            opt.textContent = g.cache?.title || g.config?.label || g.config?.url;
            sourceSel.append(opt);
        }
        sourceSel.value = grants.some(g => g.sourceId === selectedSourceId) ? selectedSourceId : "all";

        const allArticles = [];
        let latestSuccess = 0;
        for (const g of grants) {
            if (sourceSel.value !== "all" && g.sourceId !== sourceSel.value)
                continue;
            if (g.lastSuccessAt > latestSuccess)
                latestSuccess = g.lastSuccessAt;
            const feedTitle = g.cache?.title || g.config?.label || "";
            for (const item of g.cache?.items || []) {
                allArticles.push({ ...item, feedTitle });
            }
        }

        const shown = allArticles.filter(a => !unreadOnly || !readUrls.has(a.url));
        api.setMeta(state.offline ? "Offline" : `${shown.length} items`);

        const noteParts = [];
        if (state.offline)
            noteParts.push("Offline mode — showing last cached items.");
        if (statusMsg)
            noteParts.push(statusMsg);
        if (latestSuccess > 0)
            noteParts.push(`Updated ${new Date(latestSuccess).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })}`);
        statusLine.textContent = noteParts.join(" · ");
        statusLine.classList.toggle("hidden", noteParts.length === 0);

        if (shown.length === 0) {
            const empty = document.createElement("li");
            empty.className = "feed-empty";
            empty.textContent = unreadOnly ? "All items marked read." : "Press Refresh to fetch feed headlines.";
            listEl.append(empty);
            return;
        }

        const cap = tier === "compact" ? 3 : tier === "expanded" ? 20 : 8;
        for (const art of shown.slice(0, cap)) {
            const isRead = readUrls.has(art.url);
            const li = document.createElement("li");
            li.className = "dash-list-item feed-item";
            li.classList.toggle("is-read", isRead);

            const linkBtn = document.createElement("button");
            linkBtn.type = "button";
            linkBtn.className = "dash-link-btn feed-link";
            // Unread is a filled dot and a heavier title; read is a hollow ring and a lighter one.
            const mark = document.createElement("span");
            mark.className = "feed-mark";
            const label = document.createElement("span");
            label.className = "sr-only";
            label.textContent = isRead ? "Read: " : "Unread: ";

            const text = document.createElement("span");
            text.className = "feed-text";
            const title = document.createElement("span");
            title.className = "feed-title";
            title.textContent = art.title;
            title.title = art.title || "";
            const meta = document.createElement("span");
            meta.className = "dash-truncate feed-meta";
            meta.textContent = [art.feedTitle, ageText(art.publishedAt)].filter(Boolean).join(" · ");
            if (art.publishedAt)
                meta.title = art.publishedAt;
            text.append(label, title, meta);

            if (tier === "expanded" && art.excerpt) {
                const ex = document.createElement("span");
                ex.className = "feed-excerpt";
                ex.textContent = art.excerpt;
                text.append(ex);
            }
            linkBtn.append(mark, text);
            linkBtn.addEventListener("click", () => {
                if (art.url) {
                    readUrls.add(art.url);
                    render();
                    api.openUrl(art.url, true);
                }
            });

            li.append(linkBtn);
            listEl.append(li);
        }
    }

    async function refreshOne(sourceId) {
        statusMsg = "Refreshing…";
        render();
        try {
            await api.request("network.refresh", { sourceId });
            statusMsg = "";
            await load();
        } catch (err) {
            statusMsg = err.message || "Refresh failed (showing cached items).";
            render();
        }
    }

    async function refreshAll() {
        const grants = feedGrants();
        for (const g of grants)
            await refreshOne(g.sourceId);
    }

    function scheduleAutoRefresh() {
        clearInterval(autoTimer);
        const autoGrants = feedGrants().filter(g => g.config?.autoRefresh);
        if (autoGrants.length === 0 || document.hidden || state.offline)
            return;
        autoTimer = setInterval(() => {
            if (!document.hidden && !state.offline) {
                for (const g of feedGrants().filter(x => x.config?.autoRefresh))
                    refreshOne(g.sourceId);
            }
        }, 30 * 60 * 1000);
    }

    async function load() {
        state = await api.request("network.listGrants", {});
        render();
        scheduleAutoRefresh();
    }

    const offGrants = api.onEvent("networkGrantsChanged", () => load());
    const offPrivacy = api.onEvent("privacyChanged", () => load());
    const onVis = () => {
        if (document.hidden)
            clearInterval(autoTimer);
        else
            scheduleAutoRefresh();
    };
    document.addEventListener("visibilitychange", onVis);

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        renderConfig(container, close) {
            const list = document.createElement("div");
            list.className = "dash-col net-grants";
            for (const g of feedGrants()) {
                const row = document.createElement("div");
                row.className = "dash-row-between";
                const lbl = document.createElement("span");
                lbl.className = "dash-truncate";
                lbl.textContent = g.config?.url || g.sourceId;
                const rm = document.createElement("button");
                rm.type = "button";
                rm.className = "dash-btn ghost small";
                rm.textContent = "Remove & revoke";
                rm.addEventListener("click", async () => {
                    await api.request("network.revoke", { sourceId: g.sourceId });
                    await load();
                    close();
                });
                row.append(lbl, rm);
                list.append(row);
            }
            const addWrap = document.createElement("div");
            addWrap.className = "dash-col";
            renderSetup(addWrap);
            container.append(list, addWrap);
        },
        unmount() {
            clearInterval(autoTimer);
            offGrants();
            offPrivacy();
            document.removeEventListener("visibilitychange", onVis);
        },
    };
}
