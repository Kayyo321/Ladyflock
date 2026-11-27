/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #29 Keyboard Shortcuts (shortcuts-guide)
 * Live native bindings from Qt QActions + distinct user-authored reminders.
 */

import { icon } from "../icons.js";

const MODIFIER_GLYPHS = "⌃⌥⇧⌘";

// Splits a binding into the keys pressed, for drawing as keycaps. Handles native symbol-style
// strings ("⇧⌘T", "⌘+"), plus-joined strings ("Ctrl+Shift+T", "Ctrl++") and falls back to one cap.
function splitKeys(binding) {
    const str = (binding || "").trim();
    if (!str)
        return [];
    if (/[^\s+⌃⌥⇧⌘]\+/.test(str)) {
        const caps = [];
        const re = /(?:^|\+)(\+|[^+]+)/g;
        let m;
        while ((m = re.exec(str)) !== null)
            caps.push(m[1].trim());
        if (caps.length > 0 && caps.length <= 5 && caps.every(Boolean))
            return caps;
        return [str];
    }
    const caps = [];
    let i = 0;
    const chars = [...str];
    while (i < chars.length - 1 && MODIFIER_GLYPHS.includes(chars[i])) {
        caps.push(chars[i]);
        ++i;
    }
    caps.push(chars.slice(i).join(""));
    return caps;
}

export function mount(root, api) {
    let tier = api.tier;
    let nativeShortcuts = [];
    let reminders = [];
    let category = "All";

    root.classList.add("keys");

    const bar = document.createElement("div");
    bar.className = "keys-bar";
    const catSel = document.createElement("select");
    catSel.className = "dash-select keys-cat";
    catSel.setAttribute("aria-label", "Shortcut category");
    catSel.addEventListener("change", () => {
        category = catSel.value;
        render();
    });
    bar.append(catSel);

    const addRow = document.createElement("div");
    addRow.className = "keys-add";
    const lblInp = document.createElement("input");
    lblInp.type = "text";
    lblInp.className = "dash-input keys-add-label";
    lblInp.placeholder = "Personal reminder label";
    lblInp.setAttribute("aria-label", "Personal reminder label");
    const keyInp = document.createElement("input");
    keyInp.type = "text";
    keyInp.className = "dash-input mono keys-add-keys";
    keyInp.placeholder = "Keys";
    keyInp.setAttribute("aria-label", "Keys");
    const addBtn = document.createElement("button");
    addBtn.type = "button";
    addBtn.className = "dash-btn small";
    addBtn.textContent = "Add";
    addRow.append(lblInp, keyInp, addBtn);

    const listEl = document.createElement("ul");
    listEl.className = "dash-list keys-list";

    root.append(bar, listEl, addRow);

    function keycaps(binding) {
        const wrap = document.createElement("kbd");
        wrap.className = "keys-caps";
        wrap.title = binding;
        for (const key of splitKeys(binding)) {
            const cap = document.createElement("kbd");
            cap.className = "keys-cap";
            cap.textContent = key;
            wrap.append(cap);
        }
        return wrap;
    }

    function render() {
        bar.classList.toggle("hidden", tier === "compact");
        addRow.classList.toggle("hidden", tier !== "expanded");

        const cats = ["All", ...new Set(nativeShortcuts.map(s => s.category))];
        if (reminders.length > 0)
            cats.push("Personal reminders");

        catSel.replaceChildren();
        for (const c of cats) {
            const opt = document.createElement("option");
            opt.value = c;
            opt.textContent = c;
            catSel.append(opt);
        }
        if (cats.includes(category))
            catSel.value = category;
        else
            category = "All";

        listEl.replaceChildren();

        const rows = [];
        if (category !== "Personal reminders") {
            for (const s of nativeShortcuts) {
                if (category === "All" || s.category === category)
                    rows.push({ label: s.label, shortcut: s.shortcut, personal: false });
            }
        }
        if (category === "All" || category === "Personal reminders") {
            for (const r of reminders) {
                rows.push({
                    itemId: r.itemId,
                    label: r.value?.label || "Reminder",
                    shortcut: r.value?.shortcut || "",
                    personal: true,
                });
            }
        }

        api.setMeta(`${rows.length} bindings`);
        const cap = tier === "compact" ? 5 : rows.length;
        for (const r of rows.slice(0, cap)) {
            const li = document.createElement("li");
            li.className = "dash-list-item keys-item";
            li.classList.toggle("is-personal", r.personal);
            if (r.personal) {
                const mark = document.createElement("span");
                mark.className = "keys-personal-mark";
                mark.title = "Personal reminder";
                mark.append(icon("customize", 12));
                li.append(mark);
            }
            const lbl = document.createElement("span");
            lbl.className = "dash-truncate keys-label";
            lbl.textContent = r.label;
            lbl.title = r.label;
            if (r.personal) {
                const note = document.createElement("span");
                note.className = "sr-only";
                note.textContent = " (note)";
                lbl.append(note);
            }
            li.append(lbl);

            if (r.personal && tier === "expanded") {
                const actions = document.createElement("span");
                actions.className = "dash-row-actions";
                const rm = document.createElement("button");
                rm.type = "button";
                rm.className = "dash-btn ghost icon small";
                rm.setAttribute("aria-label", `Delete reminder “${r.label}”`);
                rm.title = "Delete reminder";
                rm.append(icon("close", 13));
                rm.addEventListener("click", async () => {
                    await api.request("items.delete", { collection: "shortcut-reminders", itemId: r.itemId });
                    reminders = reminders.filter(x => x.itemId !== r.itemId);
                    render();
                });
                actions.append(rm);
                li.append(actions);
            }
            li.append(keycaps(r.shortcut));
            listEl.append(li);
        }
    }

    addBtn.addEventListener("click", async () => {
        const label = lblInp.value.trim();
        const shortcut = keyInp.value.trim();
        if (!label || !shortcut)
            return;
        const val = { label: label.slice(0, 80), shortcut: shortcut.slice(0, 32) };
        const res = await api.request("items.save", { collection: "shortcut-reminders", value: val });
        reminders.push({ itemId: res.itemId, value: val });
        lblInp.value = "";
        keyInp.value = "";
        render();
    });

    async function load() {
        const [scRes, remRes] = await Promise.all([
            api.request("shortcuts.list", {}),
            api.request("items.list", { collection: "shortcut-reminders", limit: 50 }),
        ]);
        nativeShortcuts = scRes.shortcuts || [];
        reminders = remRes.items || [];
        render();
    }

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {},
    };
}
