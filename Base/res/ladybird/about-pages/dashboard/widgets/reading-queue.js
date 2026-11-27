/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #9 Reading Queue (reading-queue)
 * Offline-first reading list; duplicate URL detection offers existing item; zero network requests on add.
 */

import { icon, siteIcon } from "../icons.js";

function hostOf(url) {
    try {
        return new URL(url).hostname.replace(/^www\./, "");
    } catch {
        return "";
    }
}

export function mount(root, api) {
    let tier = api.tier;
    let items = [];
    let showFinished = false;

    root.classList.add("rq");

    // The add row reads as the next empty line of the list.
    const addRow = document.createElement("div");
    addRow.className = "rq-entry";

    const addBtn = document.createElement("button");
    addBtn.type = "button";
    addBtn.className = "rq-entry-add";
    addBtn.setAttribute("aria-label", "Add article");
    addBtn.title = "Add";
    addBtn.append(icon("plus", 13));

    const urlInput = document.createElement("input");
    urlInput.type = "url";
    urlInput.className = "rq-entry-input rq-entry-url";
    urlInput.placeholder = "Save article URL…";
    urlInput.setAttribute("aria-label", "Article URL");

    const titleInput = document.createElement("input");
    titleInput.type = "text";
    titleInput.className = "rq-entry-input rq-entry-title";
    titleInput.placeholder = "Title (optional)";
    titleInput.setAttribute("aria-label", "Article title");

    addRow.append(addBtn, urlInput, titleInput);

    const dupNotice = document.createElement("div");
    dupNotice.className = "rq-notice";
    dupNotice.setAttribute("role", "status");

    const statusBar = document.createElement("div");
    statusBar.className = "rq-footer";
    const toggleFinishedBtn = document.createElement("button");
    toggleFinishedBtn.type = "button";
    toggleFinishedBtn.className = "dash-btn ghost small";
    toggleFinishedBtn.addEventListener("click", () => {
        showFinished = !showFinished;
        render();
    });
    statusBar.append(toggleFinishedBtn);

    const listEl = document.createElement("ul");
    listEl.className = "dash-list rq-list";
    root.append(addRow, dupNotice, listEl, statusBar);

    function render() {
        titleInput.classList.toggle("hidden", tier === "compact");
        const unread = items.filter(it => !it.value?.read);
        const finished = items.filter(it => it.value?.read);
        api.setMeta(`${unread.length} unread`);
        toggleFinishedBtn.textContent = showFinished
            ? `Show unread (${unread.length})`
            : `Finished (${finished.length})`;
        toggleFinishedBtn.classList.toggle("hidden", finished.length === 0 && !showFinished);
        statusBar.classList.toggle("hidden", finished.length === 0 && !showFinished);

        const shown = showFinished ? finished : unread;
        listEl.replaceChildren();
        if (shown.length === 0) {
            const empty = document.createElement("li");
            empty.className = "rq-empty";
            empty.textContent = showFinished ? "No finished articles yet." : "Save articles to read later, offline.";
            listEl.append(empty);
            return;
        }

        for (const it of shown) {
            const li = document.createElement("li");
            li.className = "dash-list-item rq-item";
            li.classList.toggle("is-read", Boolean(it.value.read));

            const cb = document.createElement("input");
            cb.type = "checkbox";
            cb.checked = Boolean(it.value.read);
            cb.setAttribute("aria-label", `Mark ${it.value.title || it.value.url} as read`);
            cb.addEventListener("change", async () => {
                const nextVal = { ...it.value, read: cb.checked, updatedAt: Date.now() };
                const res = await api.request("items.save", {
                    collection: "reading-queue",
                    itemId: it.itemId,
                    expectedRevision: it.revision,
                    value: nextVal,
                });
                it.revision = res.revision;
                it.value = nextVal;
                render();
            });

            const linkBtn = document.createElement("button");
            linkBtn.type = "button";
            linkBtn.className = "dash-link-btn";
            linkBtn.title = it.value.url;
            const txt = document.createElement("span");
            txt.className = "dash-truncate rq-title";
            txt.textContent = it.value.title || it.value.url;
            linkBtn.append(siteIcon({ className: "dash-site-icon", url: it.value.url, label: hostOf(it.value.url) || it.value.title }), txt);
            const host = hostOf(it.value.url);
            if (tier !== "compact" && host && !(it.value.title || "").startsWith(host)) {
                const hostEl = document.createElement("span");
                hostEl.className = "dash-truncate rq-host";
                hostEl.textContent = host;
                linkBtn.append(hostEl);
            }
            linkBtn.addEventListener("click", () => api.openUrl(it.value.url, true));

            li.append(cb, linkBtn);

            if (tier !== "compact") {
                const actions = document.createElement("span");
                actions.className = "dash-row-actions";
                const delBtn = document.createElement("button");
                delBtn.type = "button";
                delBtn.className = "dash-btn ghost icon small";
                delBtn.setAttribute("aria-label", `Remove ${it.value.title || it.value.url}`);
                delBtn.title = "Remove";
                delBtn.append(icon("close", 14));
                delBtn.addEventListener("click", async () => {
                    await api.request("items.delete", { collection: "reading-queue", itemId: it.itemId });
                    items = items.filter(x => x.itemId !== it.itemId);
                    render();
                });
                actions.append(delBtn);
                li.append(actions);
            }
            listEl.append(li);
        }
    }

    async function addItem() {
        let raw = urlInput.value.trim();
        if (!raw)
            return;
        if (!/^https?:\/\//i.test(raw))
            raw = `https://${raw}`;
        let parsed;
        try {
            parsed = new URL(raw);
            if (!["http:", "https:"].includes(parsed.protocol))
                return;
        } catch {
            dupNotice.textContent = "Enter a valid HTTP(S) URL.";
            return;
        }

        const existing = items.find(it => it.value?.url === parsed.href);
        if (existing) {
            dupNotice.textContent = `Already in queue: “${existing.value.title || existing.value.url}”`;
            return;
        }

        dupNotice.textContent = "";
        const val = {
            url: parsed.href,
            title: titleInput.value.trim() || parsed.hostname.replace(/^www\./, "") + parsed.pathname,
            read: false,
            addedAt: Date.now(),
        };
        const res = await api.request("items.save", { collection: "reading-queue", value: val });
        items.unshift({ itemId: res.itemId, revision: res.revision, value: val });
        urlInput.value = "";
        titleInput.value = "";
        render();
    }

    addBtn.addEventListener("click", addItem);
    titleInput.addEventListener("keydown", e => {
        if (e.key === "Enter") {
            e.preventDefault();
            addItem();
        }
    });
    urlInput.addEventListener("keydown", e => {
        if (e.key === "Enter") {
            e.preventDefault();
            addItem();
        }
    });

    async function load() {
        const res = await api.request("items.list", { collection: "reading-queue" });
        items = res.items || [];
        render();
    }

    const offUp = api.onEvent("itemUpdated", ev => {
        if (ev.collection !== "reading-queue")
            return;
        const idx = items.findIndex(x => x.itemId === ev.itemId);
        if (idx >= 0)
            items[idx] = { itemId: ev.itemId, revision: ev.revision, value: ev.value };
        else
            items.unshift({ itemId: ev.itemId, revision: ev.revision, value: ev.value });
        render();
    });
    const offDel = api.onEvent("itemDeleted", ev => {
        if (ev.collection !== "reading-queue")
            return;
        items = items.filter(x => x.itemId !== ev.itemId);
        render();
    });

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            offUp();
            offDel();
        },
    };
}
