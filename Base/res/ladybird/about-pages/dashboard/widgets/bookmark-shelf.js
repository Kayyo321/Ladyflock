/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #6 Bookmark Shelf (bookmark-shelf)
 * Shows one bookmark folder per card instance; live updates on BookmarkStore changes.
 */

import { icon } from "../icons.js";

export function mount(root, api) {
    let config = { folderId: "root", ...api.config };
    let tier = api.tier;
    let folderData = { available: false, missing: false, folderTitle: "Bookmarks Bar", items: [] };
    let filterText = "";

    root.classList.add("shelf");

    const bar = document.createElement("div");
    bar.className = "list-entry";

    const lens = icon("search", 13);
    lens.classList.add("list-entry-glyph");
    const filterInput = document.createElement("input");
    filterInput.type = "search";
    filterInput.className = "list-entry-input";
    filterInput.placeholder = "Filter bookmarks…";
    filterInput.setAttribute("aria-label", "Filter bookmarks");
    filterInput.addEventListener("input", () => {
        filterText = filterInput.value.trim().toLowerCase();
        render();
    });

    const manageBtn = document.createElement("button");
    manageBtn.type = "button";
    manageBtn.className = "dash-btn ghost small";
    manageBtn.textContent = "Manage";
    manageBtn.setAttribute("aria-label", "Manage bookmarks");
    manageBtn.title = "Manage bookmarks";
    manageBtn.addEventListener("click", () => api.openUrl("about:bookmarks", false));

    bar.append(lens, filterInput, manageBtn);

    const listEl = document.createElement("ul");
    listEl.className = "shelf-grid";

    root.append(bar, listEl);

    function render() {
        listEl.replaceChildren();
        if (!folderData.available) {
            bar.classList.add("hidden");
            api.setMeta("Unavailable");
            const msg = document.createElement("div");
            msg.className = "dash-empty-card";
            msg.textContent = "Bookmarks are only available in the persistent default identity.";
            listEl.append(msg);
            return;
        }

        if (folderData.missing) {
            bar.classList.add("hidden");
            api.setMeta("Folder deleted");
            const wrap = document.createElement("div");
            wrap.className = "dash-empty-card";
            const p = document.createElement("span");
            p.textContent = "This bookmark folder was removed.";
            const chooseBtn = document.createElement("button");
            chooseBtn.type = "button";
            chooseBtn.className = "dash-btn";
            chooseBtn.textContent = "Use Bookmarks Bar";
            chooseBtn.addEventListener("click", async () => {
                await api.updateConfig({ folderId: "root" });
                config.folderId = "root";
                await load();
            });
            wrap.append(p, chooseBtn);
            listEl.append(wrap);
            return;
        }

        bar.classList.toggle("hidden", tier !== "expanded");
        api.setMeta(folderData.folderTitle || "Bookmarks");

        const filtered = (folderData.items || []).filter(item => {
            if (!filterText)
                return true;
            return (item.title || "").toLowerCase().includes(filterText)
                || (item.url || "").toLowerCase().includes(filterText);
        });

        if (filtered.length === 0) {
            const msg = document.createElement("div");
            msg.className = "dash-empty-card";
            msg.textContent = filterText ? "No bookmarks match that filter." : "This folder is empty.";
            listEl.append(msg);
            return;
        }

        const cap = tier === "compact" ? 4 : filtered.length;
        for (const item of filtered.slice(0, cap)) {
            const li = document.createElement("li");
            li.className = "shelf-item";

            const btn = document.createElement("button");
            btn.type = "button";
            btn.className = "shelf-link";

            const iconEl = document.createElement("span");
            iconEl.className = "shelf-icon";
            if (item.type === "folder") {
                iconEl.append(icon("folder", 16));
            } else if (item.faviconBase64Png) {
                const img = document.createElement("img");
                img.src = `data:image/png;base64,${item.faviconBase64Png}`;
                img.alt = "";
                iconEl.append(img);
            } else {
                iconEl.textContent = (item.title || "B").slice(0, 1).toUpperCase();
            }

            const lbl = document.createElement("span");
            lbl.className = "shelf-label dash-truncate";
            lbl.textContent = item.title || item.url;
            btn.title = item.title || item.url;
            if (tier === "compact")
                btn.setAttribute("aria-label", item.title || item.url);

            btn.append(iconEl);
            if (tier !== "compact")
                btn.append(lbl);

            if (item.type === "folder") {
                btn.addEventListener("click", async () => {
                    config.folderId = item.id;
                    await api.updateConfig({ folderId: item.id });
                    await load();
                });
            } else {
                btn.addEventListener("click", () => api.openUrl(item.url, false));
            }

            li.append(btn);
            listEl.append(li);
        }
    }

    async function load() {
        folderData = await api.request("bookmarks.listFolder", { folderId: config.folderId || "root" });
        render();
    }

    const offBookmarks = api.onEvent("bookmarksChanged", () => load());

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            const prevFolder = config.folderId;
            config = { folderId: "root", ...next.config };
            tier = next.tier;
            if (config.folderId !== prevFolder)
                load();
            else
                render();
        },
        renderConfig(container, close) {
            const sel = document.createElement("select");
            sel.className = "dash-select";
            const status = document.createElement("p");
            status.className = "dash-muted";
            status.textContent = "Loading folders…";

            api.request("bookmarks.listFolders", {}).then(res => {
                status.classList.add("hidden");
                for (const f of res.folders || []) {
                    const opt = document.createElement("option");
                    opt.value = f.id;
                    opt.textContent = `${f.pathLabel} (${f.childCount})`;
                    sel.append(opt);
                }
                sel.value = config.folderId || "root";
            });

            const foot = document.createElement("div");
            foot.className = "dialog-footer";
            const save = document.createElement("button");
            save.type = "button";
            save.className = "primary-button";
            save.textContent = "Save";
            save.addEventListener("click", async () => {
                await api.updateConfig({ folderId: sel.value || "root" });
                close();
            });
            foot.append(save);
            container.append(status, sel, foot);
        },
        unmount() {
            offBookmarks();
        },
    };
}
