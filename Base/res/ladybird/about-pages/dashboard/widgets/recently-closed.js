/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #4 Recently Closed (recently-closed)
 * Reopens closed tabs/windows by stable SessionWindowId.
 */

import { icon } from "../icons.js";

function splitUrl(urlStr) {
    try {
        const u = new URL(urlStr);
        const rest = `${u.pathname === "/" ? "" : u.pathname}${u.search}`;
        return { host: u.hostname.replace(/^www\./, "") || urlStr, rest };
    } catch {
        return { host: urlStr, rest: "" };
    }
}

export function mount(root, api) {
    let tier = api.tier;
    let state = { available: false, items: [] };

    const listEl = document.createElement("ul");
    listEl.className = "dash-list closed-list";
    root.classList.add("closed");
    root.append(listEl);

    function render() {
        listEl.replaceChildren();
        if (!state.available) {
            api.setMeta("Unavailable");
            const msg = document.createElement("div");
            msg.className = "dash-empty-card";
            msg.textContent = "Recently closed tabs are only available in the persistent default identity.";
            listEl.append(msg);
            return;
        }

        const cap = tier === "compact" ? 1 : tier === "expanded" ? 12 : 5;
        const shown = state.items.slice(0, cap);
        api.setMeta(`${state.items.length} saved`);

        if (shown.length === 0) {
            const msg = document.createElement("div");
            msg.className = "dash-empty-card";
            msg.textContent = "No recently closed tabs.";
            listEl.append(msg);
            return;
        }

        for (const item of shown) {
            const li = document.createElement("li");
            li.className = "dash-list-item closed-item";

            const btn = document.createElement("button");
            btn.type = "button";
            btn.className = "dash-link-btn closed-btn";
            const { host, rest } = splitUrl(item.url);
            const text = document.createElement("span");
            text.className = "closed-text";
            const primary = document.createElement("span");
            primary.className = "closed-title dash-truncate";
            primary.textContent = item.wasWindow ? `Window · ${item.tabCount} tabs` : host;
            const secondary = document.createElement("span");
            secondary.className = "closed-sub dash-truncate";
            secondary.textContent = item.wasWindow ? item.url : rest;
            btn.title = item.url;
            text.append(primary);
            if (tier !== "compact" && secondary.textContent)
                text.append(secondary);
            btn.append(text);

            if (tier === "expanded" && item.fromStartupRecovery) {
                const badge = document.createElement("span");
                badge.className = "dash-badge";
                badge.textContent = "Recovered";
                btn.append(badge);
            }

            const reopen = document.createElement("span");
            reopen.className = "closed-reopen";
            reopen.append(icon("undo", 13));
            const reopenLabel = document.createElement("span");
            reopenLabel.textContent = "Reopen";
            reopen.append(reopenLabel);
            btn.append(reopen);

            btn.addEventListener("click", async () => {
                btn.disabled = true;
                try {
                    await api.request("recentlyClosed.reopen", { unitId: item.unitId });
                } catch {
                    // Already reopened in another tab
                }
                await load();
            });

            li.append(btn);
            listEl.append(li);
        }
    }

    async function load() {
        state = await api.request("recentlyClosed.list", {});
        render();
    }

    const offClosed = api.onEvent("recentlyClosedChanged", () => load());
    const offHistory = api.onEvent("historyCleared", () => load());

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            offClosed();
            offHistory();
        },
    };
}
