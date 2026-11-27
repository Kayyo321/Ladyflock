/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #19 Scratchpad (scratchpad)
 * Local plain-text notes with independent note IDs, safe DOM-built Markdown
 * preview (no innerHTML / raw HTML / embeds), revision conflict handling,
 * and user-initiated .md export.
 */

import { icon } from "../icons.js";

function renderSafeMarkdown(container, text) {
    container.replaceChildren();
    const lines = (text || "").split(/\r?\n/);
    let inCode = false;
    let codeBuf = [];
    let currentList = null;

    const flushList = () => {
        if (currentList) {
            container.append(currentList);
            currentList = null;
        }
    };

    const appendInline = (parent, str) => {
        const parts = str.split(/(`[^`]+`|\*\*[^*]+\*\*)/g);
        for (const part of parts) {
            if (!part)
                continue;
            if (part.startsWith("`") && part.endsWith("`") && part.length > 2) {
                const code = document.createElement("code");
                code.textContent = part.slice(1, -1);
                parent.append(code);
            } else if (part.startsWith("**") && part.endsWith("**") && part.length > 4) {
                const strong = document.createElement("strong");
                strong.textContent = part.slice(2, -2);
                parent.append(strong);
            } else {
                parent.append(document.createTextNode(part));
            }
        }
    };

    for (const line of lines) {
        if (line.trim().startsWith("```")) {
            flushList();
            if (inCode) {
                const pre = document.createElement("pre");
                const code = document.createElement("code");
                code.textContent = codeBuf.join("\n");
                pre.append(code);
                container.append(pre);
                codeBuf = [];
                inCode = false;
            } else {
                inCode = true;
            }
            continue;
        }
        if (inCode) {
            codeBuf.push(line);
            continue;
        }
        const hMatch = /^(#{1,3})\s+(.*)$/.exec(line);
        if (hMatch) {
            flushList();
            const h = document.createElement(`h${hMatch[1].length}`);
            appendInline(h, hMatch[2]);
            container.append(h);
            continue;
        }
        const liMatch = /^[-*]\s+(.*)$/.exec(line);
        if (liMatch) {
            if (!currentList) {
                currentList = document.createElement("ul");
            }
            const li = document.createElement("li");
            appendInline(li, liMatch[1]);
            currentList.append(li);
            continue;
        }
        flushList();
        if (!line.trim())
            continue;
        const p = document.createElement("p");
        appendInline(p, line);
        container.append(p);
    }
    flushList();
    if (inCode && codeBuf.length) {
        const pre = document.createElement("pre");
        pre.textContent = codeBuf.join("\n");
        container.append(pre);
    }
}

export function mount(root, api) {
    let config = { noteId: "default-note", preview: false, ...api.config };
    let tier = api.tier;
    let notes = [];
    let activeNoteId = config.noteId || "default-note";
    let activeRev = 0;
    let activeTitle = "Quick note";
    let saveTimer = null;
    let dirty = false;
    let conflictState = null;

    root.classList.add("pad");

    const topBar = document.createElement("div");
    topBar.className = "pad-bar";

    const noteSelect = document.createElement("select");
    noteSelect.className = "dash-select pad-note-select";
    noteSelect.setAttribute("aria-label", "Select note");

    const btnGroup = document.createElement("div");
    btnGroup.className = "pad-actions";

    const barButton = (glyph, label) => {
        const btn = document.createElement("button");
        btn.type = "button";
        btn.className = "dash-btn ghost icon small";
        btn.setAttribute("aria-label", label);
        btn.title = label;
        btn.append(icon(glyph, 15));
        return btn;
    };

    const newNoteBtn = barButton("plus", "New note");
    const previewBtn = barButton("eye", "Preview Markdown");
    const exportBtn = barButton("external", "Export as Markdown…");

    btnGroup.append(previewBtn, newNoteBtn, exportBtn);
    topBar.append(noteSelect, btnGroup);

    const conflictBar = document.createElement("div");
    conflictBar.className = "dash-notice hidden";

    const textarea = document.createElement("textarea");
    textarea.className = "pad-paper";
    textarea.placeholder = "Write something down…";
    textarea.setAttribute("aria-label", "Scratchpad note");

    const previewEl = document.createElement("div");
    previewEl.className = "md-preview hidden";

    root.append(topBar, conflictBar, textarea, previewEl);

    function setStatus(text) {
        api.setMeta(text);
    }

    function syncVisibility() {
        const showPreview = Boolean(config.preview);
        previewBtn.setAttribute("aria-pressed", showPreview ? "true" : "false");
        previewBtn.setAttribute("aria-label", showPreview ? "Back to editing" : "Preview Markdown");
        previewBtn.title = showPreview ? "Back to editing" : "Preview Markdown";
        textarea.classList.toggle("hidden", showPreview);
        previewEl.classList.toggle("hidden", !showPreview);
        exportBtn.classList.toggle("hidden", tier === "compact");
        newNoteBtn.classList.toggle("hidden", tier === "compact");
        if (showPreview)
            renderSafeMarkdown(previewEl, textarea.value);
    }

    function renderConflict() {
        conflictBar.replaceChildren();
        if (!conflictState) {
            conflictBar.classList.add("hidden");
            return;
        }
        conflictBar.classList.remove("hidden");
        const msg = document.createElement("span");
        msg.textContent = "Edited in another tab.";
        const loadBtn = document.createElement("button");
        loadBtn.type = "button";
        loadBtn.className = "dash-btn small";
        loadBtn.textContent = "Load latest";
        loadBtn.addEventListener("click", () => {
            activeRev = conflictState.currentRevision;
            activeTitle = conflictState.currentValue?.title || activeTitle;
            textarea.value = conflictState.currentValue?.content || "";
            dirty = false;
            conflictState = null;
            renderConflict();
            syncVisibility();
            setStatus("Loaded");
        });
        const copyBtn = document.createElement("button");
        copyBtn.type = "button";
        copyBtn.className = "dash-btn primary small";
        copyBtn.textContent = "Save mine as copy";
        copyBtn.addEventListener("click", async () => {
            const copyId = `note-${Date.now().toString(36)}`;
            const copyTitle = `${activeTitle} (copy)`;
            const res = await api.request("items.save", {
                collection: "notes",
                itemId: copyId,
                value: { title: copyTitle, content: textarea.value },
            });
            activeNoteId = copyId;
            activeRev = res.revision;
            activeTitle = copyTitle;
            dirty = false;
            conflictState = null;
            renderConflict();
            await api.updateConfig({ noteId: activeNoteId });
            await loadNotes();
            setStatus("Saved copy");
        });
        conflictBar.append(msg, loadBtn, copyBtn);
    }

    function populateNoteSelect() {
        noteSelect.replaceChildren();
        const foundActive = notes.some(n => n.itemId === activeNoteId);
        if (!foundActive) {
            const opt = document.createElement("option");
            opt.value = activeNoteId;
            opt.textContent = activeTitle;
            noteSelect.append(opt);
        }
        for (const n of notes) {
            const opt = document.createElement("option");
            opt.value = n.itemId;
            opt.textContent = n.value?.title || n.itemId;
            noteSelect.append(opt);
        }
        noteSelect.value = activeNoteId;
    }

    async function flushSave() {
        clearTimeout(saveTimer);
        if (!dirty)
            return;
        const content = textarea.value;
        const maxBytes = api.context.maxNoteBytes || 65536;
        if (new TextEncoder().encode(content).length > maxBytes) {
            setStatus("Too large (64 KiB max)");
            return;
        }
        setStatus("Saving…");
        try {
            const res = await api.request("items.save", {
                collection: "notes",
                itemId: activeNoteId,
                expectedRevision: activeRev,
                value: { title: activeTitle, content },
            });
            activeRev = res.revision;
            dirty = false;
            conflictState = null;
            renderConflict();
            setStatus(api.context.isEphemeral ? "Session only" : "Saved");
        } catch (err) {
            if (err.conflict) {
                conflictState = err.payload;
                renderConflict();
                setStatus("Conflict");
            } else {
                setStatus("Couldn’t save");
            }
        }
    }

    textarea.addEventListener("input", () => {
        dirty = true;
        setStatus("Editing…");
        clearTimeout(saveTimer);
        saveTimer = setTimeout(flushSave, 450);
    });

    noteSelect.addEventListener("change", async () => {
        await flushSave();
        activeNoteId = noteSelect.value;
        await api.updateConfig({ noteId: activeNoteId });
        selectCurrentNoteFromList();
    });

    newNoteBtn.addEventListener("click", async () => {
        await flushSave();
        const newId = `note-${Date.now().toString(36)}`;
        const title = `Note ${notes.length + 1}`;
        const res = await api.request("items.save", {
            collection: "notes",
            itemId: newId,
            value: { title, content: "" },
        });
        notes.unshift({ itemId: newId, revision: res.revision, value: { title, content: "" } });
        activeNoteId = newId;
        activeRev = res.revision;
        activeTitle = title;
        textarea.value = "";
        dirty = false;
        populateNoteSelect();
        await api.updateConfig({ noteId: newId });
        textarea.focus();
    });

    previewBtn.addEventListener("click", () => {
        config.preview = !config.preview;
        api.updateConfig({ preview: config.preview });
        syncVisibility();
    });

    exportBtn.addEventListener("click", async () => {
        await flushSave();
        try {
            const res = await api.request("notes.exportMarkdown", {
                title: activeTitle,
                content: textarea.value,
            });
            if (!res.canceled)
                setStatus("Exported");
        } catch (err) {
            setStatus(err.message || "Export failed");
        }
    });

    function selectCurrentNoteFromList() {
        const found = notes.find(n => n.itemId === activeNoteId);
        if (found) {
            activeRev = found.revision;
            activeTitle = found.value?.title || "Quick note";
            textarea.value = found.value?.content || "";
        } else {
            activeRev = 0;
            activeTitle = "Quick note";
            textarea.value = "";
        }
        dirty = false;
        conflictState = null;
        renderConflict();
        populateNoteSelect();
        syncVisibility();
        setStatus(api.context.isEphemeral ? "Session only" : "Saved");
    }

    async function loadNotes() {
        const res = await api.request("items.list", { collection: "notes" });
        notes = res.items || [];
        selectCurrentNoteFromList();
    }

    const offUpdated = api.onEvent("itemUpdated", ev => {
        if (ev.collection !== "notes")
            return;
        const idx = notes.findIndex(n => n.itemId === ev.itemId);
        const rec = { itemId: ev.itemId, revision: ev.revision, value: ev.value };
        if (idx >= 0)
            notes[idx] = rec;
        else
            notes.unshift(rec);
        populateNoteSelect();
        if (ev.itemId === activeNoteId && !dirty) {
            activeRev = ev.revision;
            activeTitle = ev.value?.title || activeTitle;
            textarea.value = ev.value?.content || "";
            syncVisibility();
        }
    });

    const onBeforeUnload = () => {
        if (dirty)
            flushSave();
    };
    window.addEventListener("beforeunload", onBeforeUnload);

    loadNotes().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            config = { noteId: activeNoteId, preview: false, ...next.config };
            tier = next.tier;
            if (config.noteId && config.noteId !== activeNoteId) {
                activeNoteId = config.noteId;
                selectCurrentNoteFromList();
            } else {
                syncVisibility();
            }
        },
        renderConfig(container, close) {
            const lbl = document.createElement("label");
            lbl.textContent = "Note title";
            const titleInp = document.createElement("input");
            titleInp.type = "text";
            titleInp.className = "dash-input";
            titleInp.value = activeTitle;

            const foot = document.createElement("div");
            foot.className = "dialog-footer";
            const delBtn = document.createElement("button");
            delBtn.type = "button";
            delBtn.className = "secondary-button";
            delBtn.textContent = "Delete this note";
            delBtn.addEventListener("click", async () => {
                await api.request("items.delete", { collection: "notes", itemId: activeNoteId });
                notes = notes.filter(n => n.itemId !== activeNoteId);
                activeNoteId = notes[0]?.itemId || "default-note";
                selectCurrentNoteFromList();
                await api.updateConfig({ noteId: activeNoteId });
                close();
            });
            const saveBtn = document.createElement("button");
            saveBtn.type = "button";
            saveBtn.className = "primary-button";
            saveBtn.textContent = "Save title";
            saveBtn.addEventListener("click", async () => {
                activeTitle = titleInp.value.trim() || "Quick note";
                dirty = true;
                await flushSave();
                await loadNotes();
                close();
            });
            foot.append(delBtn, saveBtn);
            container.append(lbl, titleInp, foot);
        },
        unmount() {
            flushSave();
            offUpdated();
            window.removeEventListener("beforeunload", onBeforeUnload);
        },
    };
}
