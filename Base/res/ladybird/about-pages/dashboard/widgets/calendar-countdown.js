/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #20 Calendar and Countdowns (calendar-countdown)
 * Civil-day difference (DST-safe), leap-day/month-end safe navigation, keyboard arrows in grid.
 */

function civilDayNumber(y, m, d) {
    return Math.floor(Date.UTC(y, m - 1, d) / 86400000);
}

function todayYMD() {
    const n = new Date();
    return { y: n.getFullYear(), m: n.getMonth() + 1, d: n.getDate() };
}

function parseYMD(str) {
    const m = /^(\d{4})-(\d{2})-(\d{2})$/.exec(str || "");
    if (!m)
        return null;
    return { y: Number(m[1]), m: Number(m[2]), d: Number(m[3]) };
}

function fmtYMD(y, m, d) {
    return `${y}-${String(m).padStart(2, "0")}-${String(d).padStart(2, "0")}`;
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

function countdownText(diff) {
    if (diff === null)
        return "";
    if (diff === 0)
        return "Today";
    if (diff === 1)
        return "Tomorrow";
    if (diff === -1)
        return "Yesterday";
    return diff > 0 ? `In ${diff} days` : `${Math.abs(diff)} days ago`;
}

export function mount(root, api) {
    let config = { weekStartsMonday: false, ...api.config };
    let tier = api.tier;
    const now = todayYMD();
    let viewYear = now.y;
    let viewMonth = now.m;
    let milestones = [];
    root.classList.add("cal");

    // Compact face: today's date and the next milestone.
    const todayEl = el("div", "cal-today");

    // Month grid.
    const monthEl = el("div", "cal-month");
    const nav = el("div", "cal-nav");
    const prevBtn = el("button", "dash-btn ghost icon small");
    prevBtn.type = "button";
    prevBtn.textContent = "\u2039";
    prevBtn.setAttribute("aria-label", "Previous month");
    prevBtn.title = "Previous month";
    prevBtn.addEventListener("click", () => {
        if (--viewMonth < 1) {
            viewMonth = 12;
            --viewYear;
        }
        render();
    });

    const monthLabel = el("span", "cal-month-label");

    const nextBtn = el("button", "dash-btn ghost icon small");
    nextBtn.type = "button";
    nextBtn.textContent = "\u203A";
    nextBtn.setAttribute("aria-label", "Next month");
    nextBtn.title = "Next month";
    nextBtn.addEventListener("click", () => {
        if (++viewMonth > 12) {
            viewMonth = 1;
            ++viewYear;
        }
        render();
    });
    nav.append(monthLabel, prevBtn, nextBtn);

    const gridEl = el("div", "cal-grid");
    gridEl.setAttribute("role", "grid");
    monthEl.append(nav, gridEl);

    // Milestones.
    const sideEl = el("div", "cal-side");
    const listEl = el("ul", "dash-list cal-list");

    const addRow = el("div", "cal-add");
    const titleInp = el("input", "dash-input cal-add-title");
    titleInp.type = "text";
    titleInp.placeholder = "New milestone…";
    titleInp.setAttribute("aria-label", "Milestone title");
    const dateInp = el("input", "dash-input cal-add-date");
    dateInp.type = "date";
    dateInp.value = fmtYMD(now.y, now.m, now.d);
    dateInp.setAttribute("aria-label", "Milestone date");
    const addBtn = el("button", "dash-btn primary small cal-add-btn", "Add");
    addBtn.type = "button";
    addRow.append(titleInp, dateInp, addBtn);
    sideEl.append(listEl, addRow);

    root.append(todayEl, monthEl, sideEl);

    function daysUntil(dateStr) {
        const parsed = parseYMD(dateStr);
        if (!parsed)
            return null;
        const t = todayYMD();
        return civilDayNumber(parsed.y, parsed.m, parsed.d) - civilDayNumber(t.y, t.m, t.d);
    }

    function renderToday(sorted) {
        todayEl.replaceChildren();
        const d = new Date();
        todayEl.append(
            el("div", "dash-figure tabular cal-today-day", String(d.getDate())),
            el("div", "cal-today-date", `${new Intl.DateTimeFormat(undefined, { weekday: "long" }).format(d)} \u00B7 ${new Intl.DateTimeFormat(undefined, { month: "long" }).format(d)}`)
        );
        const next = sorted.find(m => {
            const diff = daysUntil(m.value?.date);
            return diff !== null && diff >= 0;
        });
        const line = el("div", "cal-next");
        if (next) {
            line.append(
                icon("calendar-countdown", 13),
                el("span", "dash-truncate cal-next-title", next.value?.title || "Milestone"),
                el("span", "dash-muted cal-next-when tabular", countdownText(daysUntil(next.value?.date)))
            );
        } else {
            line.append(el("span", "dash-muted", "No upcoming milestones"));
        }
        todayEl.append(line);
    }

    function render() {
        const monthName = new Intl.DateTimeFormat(undefined, { month: "long", year: "numeric" }).format(new Date(viewYear, viewMonth - 1, 1));
        monthLabel.textContent = monthName;
        const compact = tier === "compact";
        todayEl.classList.toggle("hidden", !compact);
        monthEl.classList.toggle("hidden", compact);
        sideEl.classList.toggle("hidden", compact);

        const sorted = [...milestones].sort((a, b) => (a.value?.date || "").localeCompare(b.value?.date || ""));
        api.setMeta(compact ? `${sorted.length} events` : `${sorted.length} milestones`);
        if (compact) {
            renderToday(sorted);
            return;
        }

        const eventTitles = new Map();
        for (const m of milestones) {
            const key = m.value?.date;
            if (!eventTitles.has(key))
                eventTitles.set(key, []);
            eventTitles.get(key).push(m.value?.title || "Milestone");
        }

        gridEl.replaceChildren();
        const headers = config.weekStartsMonday
            ? ["Mo", "Tu", "We", "Th", "Fr", "Sa", "Su"]
            : ["Su", "Mo", "Tu", "We", "Th", "Fr", "Sa"];
        for (const h of headers)
            gridEl.append(el("span", "cal-dow", h));

        const firstDow = new Date(viewYear, viewMonth - 1, 1).getDay();
        const offset = config.weekStartsMonday ? (firstDow + 6) % 7 : firstDow;
        const daysInMonth = new Date(viewYear, viewMonth, 0).getDate();
        const t = todayYMD();
        const longDate = new Intl.DateTimeFormat(undefined, { weekday: "long", day: "numeric", month: "long", year: "numeric" });

        for (let i = 0; i < offset; ++i)
            gridEl.append(el("span"));

        for (let d = 1; d <= daysInMonth; ++d) {
            const ymd = fmtYMD(viewYear, viewMonth, d);
            const btn = el("button", "cal-cell tabular");
            btn.type = "button";
            btn.append(el("span", "cal-day", String(d)));
            btn.dataset.day = String(d);
            const isToday = viewYear === t.y && viewMonth === t.m && d === t.d;
            const titles = eventTitles.get(ymd) || [];
            if (isToday)
                btn.classList.add("is-today");
            if (titles.length > 0) {
                btn.classList.add("has-event");
                btn.append(el("span", "cal-dot"));
            }
            let label = longDate.format(new Date(viewYear, viewMonth - 1, d));
            if (isToday)
                label += ", today";
            if (titles.length > 0)
                label += `, ${titles.length === 1 ? "1 milestone" : titles.length + " milestones"}: ${titles.join(", ")}`;
            btn.setAttribute("aria-label", label);
            btn.title = titles.length > 0 ? titles.join(", ") : label;
            btn.addEventListener("click", () => {
                dateInp.value = ymd;
            });
            btn.addEventListener("keydown", e => {
                let delta = 0;
                if (e.key === "ArrowRight")
                    delta = 1;
                else if (e.key === "ArrowLeft")
                    delta = -1;
                else if (e.key === "ArrowDown")
                    delta = 7;
                else if (e.key === "ArrowUp")
                    delta = -7;
                if (delta !== 0) {
                    e.preventDefault();
                    const targetDay = Math.max(1, Math.min(daysInMonth, d + delta));
                    const nextCell = gridEl.querySelector(`button[data-day="${targetDay}"]`);
                    nextCell?.focus();
                }
            });
            gridEl.append(btn);
        }

        listEl.replaceChildren();
        if (sorted.length === 0)
            listEl.append(el("li", "cal-empty", "No milestones yet"));
        // Upcoming first, past ones after.
        const listed = [...sorted.filter(m => daysUntil(m.value?.date) >= 0), ...sorted.filter(m => daysUntil(m.value?.date) < 0)];
        for (const item of listed.slice(0, tier === "expanded" ? 10 : 4)) {
            const diff = daysUntil(item.value?.date);
            const li = el("li", "dash-list-item cal-item" + (diff !== null && diff < 0 ? " is-past" : ""));
            const lbl = el("span", "dash-truncate cal-item-title", item.value?.title || "Milestone");
            const when = el("span", "cal-item-when tabular", countdownText(diff));
            const actions = el("span", "dash-row-actions");
            const rm = el("button", "dash-btn ghost icon small");
            rm.type = "button";
            rm.append(icon("close", 12));
            rm.setAttribute("aria-label", `Remove ${item.value?.title || "milestone"}`);
            rm.title = "Remove";
            rm.addEventListener("click", async () => {
                await api.request("items.delete", { collection: "calendar", itemId: item.itemId });
                milestones = milestones.filter(x => x.itemId !== item.itemId);
                render();
            });
            actions.append(rm);
            li.append(lbl, when, actions);
            listEl.append(li);
        }
    }

    async function addMilestone() {
        const title = titleInp.value.trim();
        const date = dateInp.value;
        if (!title || !parseYMD(date))
            return;
        const val = { title: title.slice(0, 120), date };
        const res = await api.request("items.save", { collection: "calendar", value: val });
        milestones.push({ itemId: res.itemId, revision: res.revision, value: val });
        titleInp.value = "";
        render();
    }

    addBtn.addEventListener("click", addMilestone);
    titleInp.addEventListener("keydown", e => {
        if (e.key === "Enter") {
            e.preventDefault();
            addMilestone();
        }
    });

    async function load() {
        const res = await api.request("items.list", { collection: "calendar" });
        milestones = res.items || [];
        render();
    }

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            config = { weekStartsMonday: false, ...next.config };
            tier = next.tier;
            render();
        },
        renderConfig(container, close) {
            const row = document.createElement("label");
            row.className = "dash-row";
            const cb = document.createElement("input");
            cb.type = "checkbox";
            cb.checked = Boolean(config.weekStartsMonday);
            row.append(cb, document.createTextNode("Start week on Monday"));
            const foot = document.createElement("div");
            foot.className = "dialog-footer";
            const save = document.createElement("button");
            save.type = "button";
            save.className = "primary-button";
            save.textContent = "Save";
            save.addEventListener("click", async () => {
                await api.updateConfig({ weekStartsMonday: cb.checked });
                close();
            });
            foot.append(save);
            container.append(row, foot);
        },
        unmount() {},
    };
}
