/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #27 Snippets (snippets)
 * Reusable text & command snippets. Never reads clipboard.
 * Reports "Copied" only on confirmed clipboard write; otherwise selects text for manual copy.
 */

import { icon } from "../icons.js";

export function mount(root, api) {
    let tier = api.tier;
    let items = [];
    let tagFilter = "";
    let editingId = null;

    root.classList.add("snip");

    const bar = document.createElement("div");
    bar.className = "snip-bar";
    const filterInp = document.createElement("input");
    filterInp.type = "search";
    filterInp.className = "dash-input snip-filter";
    filterInp.placeholder = "Filter by title or tag…";
    filterInp.setAttribute("aria-label", "Filter snippets");
    filterInp.addEventListener("input", () => {
        tagFilter = filterInp.value.trim().toLowerCase();
        render();
    });
    const newBtn = document.createElement("button");
    newBtn.type = "button";
    newBtn.className = "dash-btn primary small snip-new";
    newBtn.setAttribute("aria-label", "New snippet");
    newBtn.title = "New snippet";
    newBtn.append(icon("plus", 13));
    const newLbl = document.createElement("span");
    newLbl.className = "snip-new-label";
    newLbl.textContent = "New";
    newBtn.append(newLbl);
    bar.append(filterInp, newBtn);

    // A quiet form that takes the list's place while a snippet is written or edited.
    const editor = document.createElement("div");
    editor.className = "snip-editor hidden";
    const edRow = document.createElement("div");
    edRow.className = "snip-editor-row";
    const titleInp = document.createElement("input");
    titleInp.type = "text";
    titleInp.className = "dash-input snip-title-input";
    titleInp.placeholder = "Title";
    titleInp.setAttribute("aria-label", "Snippet title");
    const tagInp = document.createElement("input");
    tagInp.type = "text";
    tagInp.className = "dash-input snip-tag-input";
    tagInp.placeholder = "Tag";
    tagInp.setAttribute("aria-label", "Tag (optional, e.g. git)");
    edRow.append(titleInp, tagInp);
    const bodyInp = document.createElement("textarea");
    bodyInp.className = "dash-textarea mono snip-body-input";
    bodyInp.placeholder = "Snippet text or command…";
    bodyInp.setAttribute("aria-label", "Snippet text");
    const edActions = document.createElement("div");
    edActions.className = "snip-editor-actions";
    const saveEdBtn = document.createElement("button");
    saveEdBtn.type = "button";
    saveEdBtn.className = "dash-btn primary small";
    saveEdBtn.textContent = "Save";
    const cancelEdBtn = document.createElement("button");
    cancelEdBtn.type = "button";
    cancelEdBtn.className = "dash-btn ghost small";
    cancelEdBtn.textContent = "Cancel";
    edActions.append(cancelEdBtn, saveEdBtn);
    editor.append(edRow, bodyInp, edActions);

    const listEl = document.createElement("ul");
    listEl.className = "dash-list snip-list";

    root.append(bar, editor, listEl);

    function syncEditing() {
        const editing = !editor.classList.contains("hidden");
        listEl.classList.toggle("hidden", editing);
        root.classList.toggle("is-editing", editing);
    }

    function openEditor(snippet = null) {
        editingId = snippet ? snippet.itemId : null;
        titleInp.value = snippet?.value?.title || "";
        tagInp.value = snippet?.value?.tag || "";
        bodyInp.value = snippet?.value?.body || "";
        editor.classList.remove("hidden");
        syncEditing();
        titleInp.focus();
    }

    newBtn.addEventListener("click", () => openEditor(null));
    cancelEdBtn.addEventListener("click", () => {
        editingId = null;
        editor.classList.add("hidden");
        syncEditing();
    });

    saveEdBtn.addEventListener("click", async () => {
        const title = titleInp.value.trim();
        const body = bodyInp.value;
        if (!title || !body)
            return;
        const existing = editingId ? items.find(x => x.itemId === editingId) : null;
        const val = { title: title.slice(0, 120), tag: tagInp.value.trim().slice(0, 40), body: body.slice(0, 8192) };
        try {
            const res = await api.request("items.save", {
                collection: "snippets",
                itemId: editingId || undefined,
                expectedRevision: existing ? existing.revision : undefined,
                value: val,
            });
            if (existing) {
                existing.revision = res.revision;
                existing.value = val;
            } else {
                items.unshift({ itemId: res.itemId, revision: res.revision, value: val });
            }
            editingId = null;
            editor.classList.add("hidden");
            syncEditing();
            render();
        } catch (err) {
            if (err.conflict)
                await load();
        }
    });

    function render() {
        filterInp.classList.toggle("hidden", tier === "compact");
        const filtered = items.filter(it => {
            if (!tagFilter)
                return true;
            return (it.value?.title || "").toLowerCase().includes(tagFilter)
                || (it.value?.tag || "").toLowerCase().includes(tagFilter);
        });
        api.setMeta(`${items.length} saved`);
        listEl.replaceChildren();

        if (filtered.length === 0) {
            const empty = document.createElement("li");
            empty.className = "snip-empty";
            empty.textContent = tagFilter ? "No snippets match that filter." : "Store reusable commands and text snippets locally.";
            listEl.append(empty);
            return;
        }

        const rowButton = (glyph, label, onClick) => {
            const btn = document.createElement("button");
            btn.type = "button";
            btn.className = "dash-btn ghost icon small";
            btn.setAttribute("aria-label", label);
            btn.title = label;
            btn.append(icon(glyph, 14));
            btn.addEventListener("click", onClick);
            return btn;
        };

        for (const it of filtered) {
            const li = document.createElement("li");
            li.className = "dash-list-item snip-item";

            const text = document.createElement("div");
            text.className = "snip-text";

            const top = document.createElement("div");
            top.className = "snip-head";
            const title = document.createElement("span");
            title.className = "dash-truncate snip-title";
            title.textContent = it.value?.title || "Snippet";
            top.append(title);
            if (it.value?.tag) {
                const badge = document.createElement("span");
                badge.className = "dash-badge snip-tag";
                badge.textContent = it.value.tag;
                top.append(badge);
            }

            const preview = document.createElement("code");
            preview.className = "mono dash-truncate snip-preview";
            preview.textContent = it.value?.body || "";

            text.append(top, preview);

            const copyBtn = document.createElement("button");
            copyBtn.type = "button";
            copyBtn.className = "dash-btn small snip-copy";
            const copyLabel = `Copy “${it.value?.title || "Snippet"}”`;
            copyBtn.setAttribute("aria-label", copyLabel);
            copyBtn.title = copyLabel;
            const copyText = document.createElement("span");
            copyText.className = "snip-copy-label";
            const setCopyState = state => {
                copyBtn.classList.toggle("is-copied", state === "copied");
                copyBtn.classList.toggle("is-selected", state === "selected");
                copyBtn.replaceChildren(icon(state === "copied" ? "check" : "copy", 13), copyText);
                copyText.textContent = state === "copied" ? "Copied" : state === "selected" ? "Selected" : "Copy";
                const name = state === "copied" ? "Copied" : state === "selected" ? "Selected for manual copy" : copyLabel;
                copyBtn.setAttribute("aria-label", name);
                copyBtn.title = name;
            };
            setCopyState("idle");
            copyBtn.addEventListener("click", async () => {
                try {
                    if (!navigator.clipboard?.writeText)
                        throw new Error("Clipboard unavailable");
                    await navigator.clipboard.writeText(it.value?.body || "");
                    setCopyState("copied");
                    setTimeout(() => {
                        setCopyState("idle");
                    }, 1500);
                } catch {
                    // Clipboard denied: select visible text for manual copy.
                    const range = document.createRange();
                    range.selectNodeContents(preview);
                    const sel = window.getSelection();
                    sel.removeAllRanges();
                    sel.addRange(range);
                    setCopyState("selected");
                    setTimeout(() => {
                        setCopyState("idle");
                    }, 1800);
                }
            });

            li.append(text);

            if (tier !== "compact") {
                const actions = document.createElement("span");
                actions.className = "dash-row-actions";
                actions.append(
                    rowButton("customize", `Edit “${it.value?.title || "Snippet"}”`, () => openEditor(it)),
                    rowButton("trash", `Delete “${it.value?.title || "Snippet"}”`, async () => {
                        await api.request("items.delete", { collection: "snippets", itemId: it.itemId });
                        items = items.filter(x => x.itemId !== it.itemId);
                        render();
                    })
                );
                li.append(actions);
            }

            li.append(copyBtn);
            listEl.append(li);
        }
    }

    async function load() {
        const res = await api.request("items.list", { collection: "snippets" });
        items = res.items || [];
        render();
    }

    const offUp = api.onEvent("itemUpdated", ev => {
        if (ev.collection === "snippets")
            load();
    });

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            offUp();
        },
    };
}
