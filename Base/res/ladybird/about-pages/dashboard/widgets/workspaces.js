/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #5 Saved Workspaces (workspaces)
 * Saves eligible HTTP(S) tabs from caller's window, reporting omitted count before saving.
 * Reopens up to 30 tabs in order with partial-open status.
 */

import { icon } from "../icons.js";

export function mount(root, api) {
    let tier = api.tier;
    let items = [];
    let previewInfo = null;

    root.classList.add("spaces");

    const saveRow = document.createElement("div");
    saveRow.className = "list-entry";
    const saveBtn = document.createElement("button");
    saveBtn.type = "button";
    saveBtn.className = "list-entry-add";
    saveBtn.setAttribute("aria-label", "Save window");
    saveBtn.title = "Save window as a workspace";
    saveBtn.append(icon("plus", 12));
    const nameInput = document.createElement("input");
    nameInput.type = "text";
    nameInput.className = "list-entry-input";
    nameInput.placeholder = "Workspace name";
    nameInput.setAttribute("aria-label", "Workspace name");
    saveRow.append(saveBtn, nameInput);

    const previewNote = document.createElement("div");
    previewNote.className = "spaces-note hidden";

    const listEl = document.createElement("ul");
    listEl.className = "dash-list spaces-list";

    root.append(saveRow, previewNote, listEl);

    function render() {
        api.setMeta(tier === "compact" ? `${items.length}` : `${items.length} saved`);
        listEl.replaceChildren();

        if (previewInfo) {
            previewNote.textContent = previewInfo.omittedCount > 0
                ? `${previewInfo.eligibleCount} web tabs eligible (${previewInfo.omittedCount} internal/non-HTTP omitted).`
                : `${previewInfo.eligibleCount} web tabs eligible in this window.`;
            previewNote.classList.remove("hidden", "is-error");
            nameInput.title = previewNote.textContent;
        } else {
            previewNote.classList.add("hidden");
        }

        if (items.length === 0) {
            const empty = document.createElement("div");
            empty.className = "dash-empty-card";
            empty.textContent = "Save open web tabs from this window as a reusable workspace.";
            listEl.append(empty);
            return;
        }

        for (const ws of items) {
            const tabs = ws.value?.tabs || [];
            const li = document.createElement("li");
            li.className = "dash-list-item spaces-item";

            const openBtn = document.createElement("button");
            openBtn.type = "button";
            openBtn.className = "dash-link-btn";
            const glyph = document.createElement("span");
            glyph.className = "spaces-glyph";
            glyph.append(icon("workspaces", 14));
            const name = document.createElement("span");
            name.className = "spaces-name dash-truncate";
            name.textContent = ws.value?.name || "Workspace";
            const count = document.createElement("span");
            count.className = "spaces-count tabular";
            count.textContent = `${tabs.length} ${tabs.length === 1 ? "tab" : "tabs"}`;
            openBtn.title = "Open workspace";
            openBtn.append(glyph, name, count);

            openBtn.addEventListener("click", async () => {
                try {
                    const res = await api.request("workspaces.open", { itemId: ws.itemId });
                    const msg = res.skipped > 0
                        ? `Opened ${res.opened} tabs (${res.skipped} skipped).`
                        : `Opened ${res.opened} tabs.`;
                    api.announce(msg);
                } catch (err) {
                    api.announce(err.message || "Couldn’t open workspace.");
                }
            });

            li.append(openBtn);

            if (tier !== "compact") {
                const delBtn = document.createElement("button");
                delBtn.type = "button";
                delBtn.className = "dash-btn ghost icon small";
                delBtn.title = "Delete workspace";
                delBtn.append(icon("trash", 13));
                delBtn.setAttribute("aria-label", `Delete workspace ${ws.value?.name || ""}`);
                delBtn.addEventListener("click", async () => {
                    await api.request("items.delete", { collection: "workspaces", itemId: ws.itemId });
                    items = items.filter(x => x.itemId !== ws.itemId);
                    render();
                });
                const actions = document.createElement("div");
                actions.className = "dash-row-actions";
                actions.append(delBtn);
                li.append(actions);
            }
            listEl.append(li);
        }
    }

    nameInput.addEventListener("keydown", e => {
        if (e.key === "Enter") {
            e.preventDefault();
            saveBtn.click();
        }
    });

    saveBtn.addEventListener("click", async () => {
        const name = nameInput.value.trim() || `Workspace ${items.length + 1}`;
        try {
            const res = await api.request("workspaces.saveFromWindow", { name });
            nameInput.value = "";
            if (res.omittedCount > 0)
                api.announce(`Saved workspace (${res.omittedCount} non-web tabs omitted).`);
            await load();
        } catch (err) {
            previewNote.textContent = err.message || "Couldn’t save workspace.";
            previewNote.classList.remove("hidden");
            previewNote.classList.add("is-error");
        }
    });

    async function load() {
        const [listRes, prevRes] = await Promise.all([
            api.request("items.list", { collection: "workspaces" }),
            api.request("workspaces.saveFromWindow", { previewOnly: true }).catch(() => null),
        ]);
        items = listRes.items || [];
        previewInfo = prevRes;
        render();
    }

    const offCol = api.onEvent("collectionChanged", ev => {
        if (ev.collection === "workspaces")
            load();
    });

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            offCol();
        },
    };
}
