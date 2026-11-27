/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #18 Quick Tasks (quick-tasks)
 * Shared collection across cards; supports named list filter, inline editing,
 * keyboard/button reordering, and filtered clear-completed.
 */

import { icon } from "../icons.js";

export function mount(root, api) {
    let config = { listFilter: "", ...api.config };
    let tier = api.tier;
    let items = []; // [{ itemId, revision, value: { text, done, list, order } }]

    root.classList.add("tasks");

    // The entry row reads as the next empty line of the list.
    const formRow = document.createElement("div");
    formRow.className = "tasks-entry";

    const addBtn = document.createElement("button");
    addBtn.type = "button";
    addBtn.className = "tasks-entry-add";
    addBtn.setAttribute("aria-label", "Add task");
    addBtn.append(icon("plus", 13));

    const input = document.createElement("input");
    input.type = "text";
    input.className = "tasks-entry-input";
    input.placeholder = "New task";
    input.setAttribute("aria-label", "New task");

    formRow.append(addBtn, input);

    const listEl = document.createElement("ul");
    listEl.className = "dash-list tasks-list";

    const filterRow = document.createElement("div");
    filterRow.className = "tasks-footer";

    const listSelect = document.createElement("select");
    listSelect.className = "dash-select tasks-list-select";
    listSelect.setAttribute("aria-label", "Filter task list");

    const clearDoneBtn = document.createElement("button");
    clearDoneBtn.type = "button";
    clearDoneBtn.className = "dash-btn ghost small";
    clearDoneBtn.textContent = "Clear completed";

    filterRow.append(listSelect, clearDoneBtn);

    root.append(formRow, listEl, filterRow);

    function visibleItems() {
        const activeList = config.listFilter || "";
        return items
            .filter(it => !activeList || (it.value.list || "Inbox") === activeList)
            .sort((a, b) => (a.value.order ?? 0) - (b.value.order ?? 0) || a.itemId.localeCompare(b.itemId));
    }

    function render() {
        const allLists = new Set(["Inbox"]);
        for (const it of items)
            allLists.add(it.value.list || "Inbox");
        if (config.listFilter)
            allLists.add(config.listFilter);

        listSelect.replaceChildren();
        const allOpt = document.createElement("option");
        allOpt.value = "";
        allOpt.textContent = "All lists";
        listSelect.append(allOpt);
        for (const name of [...allLists].sort()) {
            const opt = document.createElement("option");
            opt.value = name;
            opt.textContent = name;
            listSelect.append(opt);
        }
        listSelect.value = config.listFilter || "";

        const vis = visibleItems();
        const doneVis = vis.filter(it => it.value.done);
        const remaining = vis.length - doneVis.length;
        api.setMeta(vis.length === 0 ? "" : remaining === 0 ? "All done" : `${remaining} left`);
        clearDoneBtn.disabled = doneVis.length === 0;
        clearDoneBtn.classList.toggle("hidden", doneVis.length === 0);
        // The list switcher only earns its place once there is more than one list.
        listSelect.classList.toggle("hidden", allLists.size <= 1);
        filterRow.classList.toggle("hidden", allLists.size <= 1 && doneVis.length === 0);

        listEl.replaceChildren();
        if (vis.length === 0) {
            const empty = document.createElement("li");
            empty.className = "tasks-empty";
            empty.textContent = config.listFilter ? `Nothing in “${config.listFilter}”.` : "Nothing to do.";
            listEl.append(empty);
            return;
        }

        const rowButton = (glyph, label, onClick, disabled = false) => {
            const btn = document.createElement("button");
            btn.type = "button";
            btn.className = "dash-btn ghost icon small";
            btn.disabled = disabled;
            btn.setAttribute("aria-label", label);
            btn.title = label;
            btn.append(icon(glyph, 14));
            btn.addEventListener("click", onClick);
            return btn;
        };

        vis.forEach((it, idx) => {
            const li = document.createElement("li");
            li.className = "dash-list-item tasks-item";
            li.classList.toggle("is-done", Boolean(it.value.done));

            const cb = document.createElement("input");
            cb.type = "checkbox";
            cb.checked = Boolean(it.value.done);
            cb.setAttribute("aria-label", `Mark "${it.value.text}" complete`);
            cb.addEventListener("change", () => saveTask(it, { ...it.value, done: cb.checked }));

            const textSpan = document.createElement("span");
            textSpan.className = "dash-truncate dash-grow tasks-text";
            textSpan.textContent = it.value.text || "";
            textSpan.title = it.value.text || "";

            li.append(cb, textSpan);

            if (tier !== "compact") {
                const actions = document.createElement("span");
                actions.className = "dash-row-actions";
                actions.append(
                    rowButton("arrow-up", `Move "${it.value.text}" up`, () => swapOrder(vis[idx - 1], it), idx === 0),
                    rowButton("arrow-down", `Move "${it.value.text}" down`, () => swapOrder(it, vis[idx + 1]), idx === vis.length - 1),
                    rowButton("close", `Delete "${it.value.text}"`, async () => {
                        await api.request("items.delete", { collection: "tasks", itemId: it.itemId });
                        items = items.filter(x => x.itemId !== it.itemId);
                        render();
                    })
                );
                li.append(actions);
            }
            listEl.append(li);
        });
    }

    async function saveTask(existing, nextValue) {
        try {
            const res = await api.request("items.save", {
                collection: "tasks",
                itemId: existing.itemId,
                expectedRevision: existing.revision,
                value: nextValue,
            });
            existing.revision = res.revision;
            existing.value = nextValue;
            render();
        } catch (err) {
            if (err.conflict)
                await load();
        }
    }

    async function swapOrder(a, b) {
        const orderA = a.value.order ?? 0;
        const orderB = b.value.order ?? 1;
        const newOrderA = orderA === orderB ? orderB + 1 : orderB;
        const newOrderB = orderA;
        await saveTask(a, { ...a.value, order: newOrderA });
        await saveTask(b, { ...b.value, order: newOrderB });
        render();
    }

    async function addTask() {
        const text = input.value.trim();
        if (!text)
            return;
        input.value = "";
        const maxOrder = items.reduce((m, it) => Math.max(m, it.value.order ?? 0), 0);
        const value = {
            text: text.slice(0, 280),
            done: false,
            list: config.listFilter || "Inbox",
            order: maxOrder + 1,
        };
        const res = await api.request("items.save", { collection: "tasks", value });
        items.push({ itemId: res.itemId, revision: res.revision, value });
        render();
    }

    addBtn.addEventListener("click", addTask);
    input.addEventListener("keydown", e => {
        if (e.key === "Enter") {
            e.preventDefault();
            addTask();
        }
    });

    listSelect.addEventListener("change", () => {
        config.listFilter = listSelect.value;
        api.updateConfig({ listFilter: config.listFilter });
        render();
    });

    clearDoneBtn.addEventListener("click", async () => {
        const ids = visibleItems().filter(it => it.value.done).map(it => it.itemId);
        if (!ids.length)
            return;
        await api.request("items.clearCompleted", { collection: "tasks", itemIds: ids });
        const idSet = new Set(ids);
        items = items.filter(it => !idSet.has(it.itemId));
        render();
    });

    async function load() {
        const res = await api.request("items.list", { collection: "tasks" });
        items = res.items || [];
        render();
    }

    const offUpdate = api.onEvent("itemUpdated", ev => {
        if (ev.collection !== "tasks")
            return;
        const idx = items.findIndex(x => x.itemId === ev.itemId);
        if (idx >= 0)
            items[idx] = { itemId: ev.itemId, revision: ev.revision, value: ev.value };
        else
            items.push({ itemId: ev.itemId, revision: ev.revision, value: ev.value });
        render();
    });

    const offDelete = api.onEvent("itemDeleted", ev => {
        if (ev.collection !== "tasks")
            return;
        items = items.filter(x => x.itemId !== ev.itemId);
        render();
    });

    const offCol = api.onEvent("collectionChanged", ev => {
        if (ev.collection === "tasks")
            load();
    });

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            config = { listFilter: "", ...next.config };
            tier = next.tier;
            render();
        },
        renderConfig(container, close) {
            const label = document.createElement("label");
            label.textContent = "Default list filter (leave blank for All lists)";
            const inp = document.createElement("input");
            inp.type = "text";
            inp.className = "dash-input";
            inp.value = config.listFilter || "";
            inp.placeholder = "e.g. Work, Inbox";

            const foot = document.createElement("div");
            foot.className = "dialog-footer";
            const save = document.createElement("button");
            save.type = "button";
            save.className = "primary-button";
            save.textContent = "Save";
            save.addEventListener("click", async () => {
                await api.updateConfig({ listFilter: inp.value.trim() });
                close();
            });
            foot.append(save);
            container.append(label, inp, foot);
        },
        unmount() {
            offUpdate();
            offDelete();
            offCol();
        },
    };
}
