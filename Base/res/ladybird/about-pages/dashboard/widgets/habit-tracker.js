/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #21 Habit Tracker (habit-tracker)
 * Stores local calendar YYYY-MM-DD check-ins uniquely per habit/day; computes consecutive streaks.
 */

function localDateKey(daysOffset = 0) {
    const d = new Date();
    d.setDate(d.getDate() + daysOffset);
    return `${d.getFullYear()}-${String(d.getMonth() + 1).padStart(2, "0")}-${String(d.getDate()).padStart(2, "0")}`;
}

function computeStreak(datesSet) {
    let streak = 0;
    let offset = datesSet.has(localDateKey(0)) ? 0 : -1;
    while (datesSet.has(localDateKey(offset))) {
        ++streak;
        --offset;
    }
    return streak;
}

import { icon } from "../icons.js";

function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className)
        node.className = className;
    if (text !== undefined)
        node.textContent = text;
    return node;
}

export function mount(root, api) {
    let tier = api.tier;
    let habits = [];
    root.classList.add("habits");

    const addRow = el("div", "habits-add");
    const inp = el("input", "dash-input habits-add-input");
    inp.type = "text";
    inp.placeholder = "New habit…";
    inp.setAttribute("aria-label", "New habit");
    const addBtn = el("button", "dash-btn primary small", "Add");
    addBtn.type = "button";
    addRow.append(inp, addBtn);

    const listEl = el("ul", "dash-list habits-list");
    root.append(addRow, listEl);

    function render() {
        const today = localDateKey(0);
        const doneToday = habits.filter(h => (h.value?.dates || []).includes(today)).length;
        api.setMeta(`${doneToday}/${habits.length} today`);
        addRow.classList.toggle("hidden", tier === "compact");

        listEl.replaceChildren();
        if (habits.length === 0) {
            listEl.append(el("li", "habits-empty", "Add a daily habit to track streaks locally."));
            return;
        }

        const spanDays = tier === "expanded" ? 28 : tier === "default" ? 7 : 0;

        for (const h of habits) {
            const name = h.value?.name || "Habit";
            const dates = new Set(h.value?.dates || []);
            const checkedToday = dates.has(today);
            const streak = computeStreak(dates);

            const li = el("li", "dash-list-item habit" + (checkedToday ? " is-done" : ""));

            const toggle = el("button", "habit-toggle");
            toggle.type = "button";
            toggle.setAttribute("aria-pressed", String(checkedToday));
            toggle.setAttribute("aria-label", `${name}: done today`);
            toggle.title = checkedToday ? "Mark as not done" : "Mark as done today";
            toggle.append(icon("check", 14));
            toggle.addEventListener("click", () => toggleToday(h, !checkedToday));

            const title = el("span", "habit-name dash-truncate", name);
            li.append(toggle, title);

            if (tier === "default") {
                li.append(buildDots(name, dates, spanDays));
            }

            const streakEl = el("span", "habit-streak tabular" + (streak === 0 ? " is-zero" : ""));
            streakEl.append(el("b", "", String(streak)), document.createTextNode(streak === 1 ? " day" : " days"));
            streakEl.title = "Current streak";
            li.append(streakEl);

            if (tier !== "compact") {
                const actions = el("span", "dash-row-actions");
                const rm = el("button", "dash-btn ghost icon small");
                rm.type = "button";
                rm.append(icon("close", 12));
                rm.setAttribute("aria-label", `Remove ${name}`);
                rm.title = "Remove";
                rm.addEventListener("click", async () => {
                    await api.request("items.delete", { collection: "habits", itemId: h.itemId });
                    habits = habits.filter(x => x.itemId !== h.itemId);
                    render();
                });
                actions.append(rm);
                li.append(actions);
            }

            if (tier === "expanded")
                li.append(buildDots(name, dates, spanDays));

            listEl.append(li);
        }
    }

    // A strip (default) or grid (expanded) of one dot per day, oldest first, today last.
    function buildDots(name, dates, spanDays) {
        const cells = el("div", "habit-dots" + (spanDays > 7 ? " is-grid" : ""));
        cells.setAttribute("role", "img");
        let hits = 0;
        for (let d = -(spanDays - 1); d <= 0; ++d) {
            const key = localDateKey(d);
            const done = dates.has(key);
            if (done)
                ++hits;
            const dot = el("span", "habit-dot" + (done ? " done" : "") + (d === 0 ? " is-today" : ""));
            dot.title = `${key}: ${done ? "Done" : "Missed"}`;
            cells.append(dot);
        }
        cells.setAttribute("aria-label", `${name}: ${hits} of last ${spanDays} days completed.`);
        return cells;
    }

    async function toggleToday(habit, done) {
        const today = localDateKey(0);
        const set = new Set(habit.value?.dates || []);
        if (done)
            set.add(today);
        else
            set.delete(today);
        const nextVal = { ...habit.value, dates: [...set].sort() };
        try {
            const res = await api.request("items.save", {
                collection: "habits",
                itemId: habit.itemId,
                expectedRevision: habit.revision,
                value: nextVal,
            });
            habit.revision = res.revision;
            habit.value = nextVal;
            render();
        } catch (err) {
            if (err.conflict)
                await load();
        }
    }

    async function addHabit() {
        const name = inp.value.trim();
        if (!name)
            return;
        const val = { name: name.slice(0, 80), dates: [] };
        const res = await api.request("items.save", { collection: "habits", value: val });
        habits.push({ itemId: res.itemId, revision: res.revision, value: val });
        inp.value = "";
        render();
    }

    addBtn.addEventListener("click", addHabit);
    inp.addEventListener("keydown", e => {
        if (e.key === "Enter") {
            e.preventDefault();
            addHabit();
        }
    });

    async function load() {
        const res = await api.request("items.list", { collection: "habits" });
        habits = res.items || [];
        render();
    }

    const offUp = api.onEvent("itemUpdated", ev => {
        if (ev.collection === "habits")
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
