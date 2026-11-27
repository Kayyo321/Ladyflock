/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #1 Top Sites (top-sites)
 * Explicit pins stored separately from history + bounded visit-score inferred sites.
 */

import { icon, siteIcon } from "../icons.js";

function hostLabel(urlStr) {
    try {
        return new URL(urlStr).hostname.replace(/^www\./, "");
    } catch {
        return urlStr;
    }
}

export function mount(root, api) {
    let tier = api.tier;
    let data = { pins: [], inferred: [], historyAvailable: false };

    root.classList.add("sites");

    const gridEl = document.createElement("div");
    gridEl.className = "sites-grid";

    const addRow = document.createElement("div");
    addRow.className = "sites-add";
    const addBtn = document.createElement("button");
    addBtn.type = "button";
    addBtn.className = "sites-add-btn";
    addBtn.setAttribute("aria-label", "Pin site");
    addBtn.title = "Pin site";
    addBtn.append(icon("plus", 12));
    const urlInput = document.createElement("input");
    urlInput.type = "url";
    urlInput.className = "sites-add-input";
    urlInput.placeholder = "Pin a site…";
    urlInput.setAttribute("aria-label", "Pin site URL");
    addRow.append(addBtn, urlInput);

    root.append(gridEl, addRow);

    function actionButton(glyph, label, handler) {
        const b = document.createElement("button");
        b.type = "button";
        b.className = "sites-action";
        b.setAttribute("aria-label", label);
        b.title = label;
        b.append(icon(glyph, 11));
        b.addEventListener("click", handler);
        return b;
    }

    function render() {
        gridEl.replaceChildren();
        const maxItems = tier === "compact" ? 4 : tier === "expanded" ? 12 : 8;
        const combined = [...data.pins, ...data.inferred].slice(0, maxItems);
        api.setMeta(`${combined.length} sites`);
        addRow.classList.toggle("hidden", tier === "compact");

        if (combined.length === 0) {
            const empty = document.createElement("div");
            empty.className = "dash-empty-card";
            empty.textContent = data.historyAvailable
                ? "Visit sites or pin your favorites here."
                : "Pin favorite sites for quick access.";
            gridEl.append(empty);
            return;
        }

        for (const site of combined) {
            const name = site.title || hostLabel(site.url);
            const tile = document.createElement("div");
            tile.className = "sites-tile";

            const linkBtn = document.createElement("button");
            linkBtn.type = "button";
            linkBtn.className = "sites-link";
            linkBtn.title = tier === "compact" ? `${name} · ${hostLabel(site.url)}` : hostLabel(site.url);
            if (tier === "compact")
                linkBtn.setAttribute("aria-label", name);
            linkBtn.addEventListener("click", () => api.openUrl(site.url, false));

            const iconEl = siteIcon({ className: "sites-icon", url: site.url, label: name, favicon: site.faviconBase64Png });
            if (site.pinned && tier !== "compact") {
                const mark = document.createElement("span");
                mark.className = "sites-pin-mark";
                mark.append(icon("pin", 9));
                iconEl.append(mark);
            }
            linkBtn.append(iconEl);

            if (tier !== "compact") {
                const title = document.createElement("span");
                title.className = "sites-name dash-truncate";
                title.textContent = name;
                linkBtn.append(title);
            }

            // The domain line is only worth showing when the name says something else.
            if (tier === "expanded" && hostLabel(site.url) !== name) {
                const domain = document.createElement("span");
                domain.className = "sites-domain dash-truncate";
                domain.textContent = hostLabel(site.url);
                linkBtn.append(domain);
            }

            tile.append(linkBtn);

            if (tier === "expanded") {
                const actions = document.createElement("div");
                actions.className = "sites-actions";
                if (site.pinned) {
                    actions.append(actionButton("close", `Unpin ${name}`, async () => {
                        await api.request("items.delete", { collection: "top-sites-pins", itemId: site.itemId });
                        await load();
                    }));
                } else {
                    actions.append(
                        actionButton("pin", `Pin ${name}`, async () => {
                            await api.request("items.save", {
                                collection: "top-sites-pins",
                                value: { url: site.url, title: site.title || hostLabel(site.url) },
                            });
                            await load();
                        }),
                        actionButton("close", `Dismiss ${name}`, async () => {
                            await api.request("items.save", {
                                collection: "top-sites-dismissed",
                                value: { url: site.url },
                            });
                            await load();
                        }));
                }
                tile.append(actions);
            }

            gridEl.append(tile);
        }
    }

    async function pinInputUrl() {
        let raw = urlInput.value.trim();
        if (!raw)
            return;
        if (!/^https?:\/\//i.test(raw))
            raw = `https://${raw}`;
        try {
            const parsed = new URL(raw);
            if (!["http:", "https:"].includes(parsed.protocol))
                return;
            await api.request("items.save", {
                collection: "top-sites-pins",
                value: { url: parsed.href, title: parsed.hostname.replace(/^www\./, "") },
            });
            urlInput.value = "";
            await load();
        } catch {
            // Invalid URL; keep focus
        }
    }

    addBtn.addEventListener("click", pinInputUrl);
    urlInput.addEventListener("keydown", e => {
        if (e.key === "Enter") {
            e.preventDefault();
            pinInputUrl();
        }
    });

    async function load() {
        data = await api.request("topSites.load", {});
        render();
    }

    const offHistory = api.onEvent("historyCleared", () => load());
    const offItems = api.onEvent("itemUpdated", ev => {
        if (ev.collection === "top-sites-pins" || ev.collection === "top-sites-dismissed")
            load();
    });

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            offHistory();
            offItems();
        },
    };
}
