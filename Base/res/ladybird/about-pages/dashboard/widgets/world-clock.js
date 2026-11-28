/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #16 World Clock (world-clock)
 * Local time and date over a ribbon of the day, labelled IANA time zones, working-hours indicator.
 * Wakes on minute boundaries unless visible seconds are enabled.
 */

import { icon } from "../icons.js";

const COMMON_ZONES = [
    { label: "UTC", tz: "UTC" },
    { label: "New York", tz: "America/New_York" },
    { label: "San Francisco", tz: "America/Los_Angeles" },
    { label: "London", tz: "Europe/London" },
    { label: "Berlin", tz: "Europe/Berlin" },
    { label: "Tokyo", tz: "Asia/Tokyo" },
    { label: "Sydney", tz: "Australia/Sydney" },
    { label: "Bengaluru", tz: "Asia/Kolkata" },
];

function isValidTimeZone(tz) {
    if (!tz || typeof tz !== "string")
        return false;
    try {
        new Intl.DateTimeFormat(undefined, { timeZone: tz });
        return true;
    } catch {
        return false;
    }
}

export function mount(root, api) {
    let config = { use24Hour: false, showSeconds: false, zones: [], ...api.config };
    let tier = api.tier;
    let timerId = null;

    const faceEl = document.createElement("div");
    faceEl.className = "clock-face";

    const primaryEl = document.createElement("div");
    primaryEl.className = "dash-figure clock-time";

    const dateEl = document.createElement("div");
    dateEl.className = "clock-date";

    // The day ribbon: one bar for the 24 hours of today, midnight to midnight, with a marker for now.
    const ribbon = createRibbon();
    const dayEl = document.createElement("div");
    dayEl.className = "clock-day";
    dayEl.setAttribute("aria-hidden", "true");
    const marksEl = document.createElement("div");
    marksEl.className = "clock-day-marks";
    marksEl.append(icon("moon", 11), icon("sun", 12), icon("moon", 11));
    dayEl.append(marksEl, ribbon.el);

    faceEl.append(primaryEl, dateEl);

    const zonesEl = document.createElement("div");
    zonesEl.className = "clock-zones";

    root.append(faceEl, zonesEl, dayEl);

    function createRibbon() {
        const el = document.createElement("div");
        el.className = "clock-ribbon";
        const knob = document.createElement("span");
        knob.className = "clock-ribbon-knob";
        el.append(knob);
        return {
            el,
            set(minutesOfDay) {
                knob.style.insetInlineStart = `${(Math.max(0, Math.min(1439, minutesOfDay)) / 1440) * 100}%`;
            },
        };
    }

    function timeParts(date, tz) {
        const opts = {
            hour: "numeric",
            minute: "2-digit",
            hour12: !config.use24Hour,
        };
        if (config.use24Hour)
            opts.hour = "2-digit";
        if (config.showSeconds && tier !== "compact")
            opts.second = "2-digit";
        if (tz)
            opts.timeZone = tz;
        try {
            const parts = new Intl.DateTimeFormat(undefined, opts).formatToParts(date);
            const period = parts.find(p => p.type === "dayPeriod")?.value || "";
            const clock = parts
                .filter(p => p.type !== "dayPeriod")
                .map(p => p.value)
                .join("")
                .trim();
            return { clock, period };
        } catch {
            return { clock: "Invalid zone", period: "" };
        }
    }

    function formatZoneTime(date, tz) {
        const { clock, period } = timeParts(date, tz);
        return period ? `${clock} ${period}` : clock;
    }

    function getZoneMinutes(date, tz) {
        try {
            const opts = { hour: "numeric", minute: "numeric", hour12: false };
            if (tz)
                opts.timeZone = tz;
            const parts = new Intl.DateTimeFormat("en-US", opts).formatToParts(date);
            const h = Number.parseInt(parts.find(p => p.type === "hour")?.value || "0", 10) % 24;
            const m = Number.parseInt(parts.find(p => p.type === "minute")?.value || "0", 10);
            return h * 60 + m;
        } catch {
            return null;
        }
    }

    function shortZoneName(date) {
        try {
            const parts = new Intl.DateTimeFormat(undefined, { timeZoneName: "short" }).formatToParts(date);
            return parts.find(p => p.type === "timeZoneName")?.value || "";
        } catch {
            return "";
        }
    }

    function render() {
        const now = new Date();
        const { clock, period } = timeParts(now, undefined);
        primaryEl.replaceChildren(document.createTextNode(clock));
        if (period) {
            const unit = document.createElement("span");
            unit.className = "dash-figure-unit";
            unit.textContent = period;
            primaryEl.append(unit);
        }
        const date = new Intl.DateTimeFormat(undefined, {
            weekday: tier === "compact" ? "short" : "long",
            month: tier === "compact" ? "short" : "long",
            day: "numeric",
        }).format(now);
        const zone = shortZoneName(now) || Intl.DateTimeFormat().resolvedOptions().timeZone || "";
        dateEl.textContent = zone ? `${date} · ${zone}` : date;

        const localMinutes = getZoneMinutes(now, undefined);
        dayEl.classList.toggle("hidden", tier === "compact" || localMinutes === null);
        if (localMinutes !== null)
            ribbon.set(localMinutes);

        zonesEl.replaceChildren();
        const zones = Array.isArray(config.zones) ? config.zones : [];
        zonesEl.classList.toggle("hidden", tier === "compact" || zones.length === 0);
        if (tier !== "compact") {
            for (const z of zones) {
                const valid = isValidTimeZone(z.tz);
                const row = document.createElement("div");
                row.className = "clock-zone";

                const lbl = document.createElement("span");
                lbl.className = "clock-zone-name dash-truncate";
                lbl.textContent = z.label || z.tz;

                const tEl = document.createElement("span");
                tEl.className = "clock-zone-time tabular";
                tEl.textContent = valid ? formatZoneTime(now, z.tz) : "Invalid zone";

                row.append(lbl);

                const minutes = valid ? getZoneMinutes(now, z.tz) : null;
                if (tier === "expanded" && minutes !== null) {
                    const zoneRibbon = createRibbon();
                    zoneRibbon.el.classList.add("small");
                    zoneRibbon.el.setAttribute("aria-hidden", "true");
                    zoneRibbon.set(minutes);
                    const working = minutes >= 9 * 60 && minutes < 18 * 60;
                    const badge = document.createElement("span");
                    badge.className = `dash-badge ${working ? "ok" : ""}`;
                    badge.textContent = working ? "Work hrs" : "Off hrs";
                    row.append(zoneRibbon.el, badge);
                }

                row.append(tEl);
                zonesEl.append(row);
            }
        }
    }

    function reschedule() {
        clearTimeout(timerId);
        if (document.hidden)
            return;
        const now = Date.now();
        const showSec = config.showSeconds && tier !== "compact";
        const delay = showSec ? (1000 - (now % 1000) + 15) : (60000 - (now % 60000) + 50);
        timerId = setTimeout(() => {
            render();
            reschedule();
        }, delay);
    }

    const onVisibility = () => {
        if (!document.hidden) {
            render();
            reschedule();
        } else {
            clearTimeout(timerId);
        }
    };
    document.addEventListener("visibilitychange", onVisibility);

    render();
    reschedule();

    return {
        onUpdate(next) {
            config = { use24Hour: false, showSeconds: false, zones: [], ...next.config };
            tier = next.tier;
            render();
            reschedule();
        },
        renderConfig(container, close) {
            let draft24Hour = Boolean(config.use24Hour);
            let draftSeconds = Boolean(config.showSeconds);
            const makeSwitch = (labelText, checked, onChange) => {
                const row = document.createElement("label");
                row.className = "dash-switch-row";
                const text = document.createElement("span");
                text.textContent = labelText;
                const input = document.createElement("input");
                input.type = "checkbox";
                input.setAttribute("switch", "");
                input.checked = checked;
                input.addEventListener("change", () => onChange(input.checked));
                row.append(text, input);
                return row;
            };
            const hourSwitch = makeSwitch("24-hour time", draft24Hour, v => {
                draft24Hour = v;
            });
            const secondsSwitch = makeSwitch("Show seconds", draftSeconds, v => {
                draftSeconds = v;
            });

            const zonesLabel = document.createElement("div");
            zonesLabel.className = "dash-label";
            zonesLabel.textContent = "Other time zones";

            const list = document.createElement("div");
            list.className = "dash-col";

            let draftZones = Array.isArray(config.zones) ? config.zones.map(z => ({ ...z })) : [];

            const renderDraft = () => {
                list.replaceChildren();
                for (let i = 0; i < draftZones.length; ++i) {
                    const z = draftZones[i];
                    const row = document.createElement("div");
                    row.className = "dash-row-between";
                    const span = document.createElement("span");
                    span.textContent = `${z.label || z.tz} (${z.tz})`;
                    const rm = document.createElement("button");
                    rm.type = "button";
                    rm.className = "dash-btn ghost";
                    rm.textContent = "Remove";
                    rm.addEventListener("click", () => {
                        draftZones.splice(i, 1);
                        renderDraft();
                    });
                    row.append(span, rm);
                    list.append(row);
                }
            };
            renderDraft();

            const addRow = document.createElement("div");
            addRow.className = "dash-row";
            const sel = document.createElement("select");
            sel.className = "dash-select";
            sel.style.flex = "1";
            for (const cz of COMMON_ZONES) {
                const opt = document.createElement("option");
                opt.value = cz.tz;
                opt.textContent = `${cz.label} (${cz.tz})`;
                sel.append(opt);
            }
            const addBtn = document.createElement("button");
            addBtn.type = "button";
            addBtn.className = "dash-btn";
            addBtn.textContent = "Add zone";
            addBtn.addEventListener("click", () => {
                const chosen = COMMON_ZONES.find(c => c.tz === sel.value);
                if (chosen && draftZones.length < 8) {
                    draftZones.push({ label: chosen.label, tz: chosen.tz });
                    renderDraft();
                }
            });
            addRow.append(sel, addBtn);

            const customRow = document.createElement("div");
            customRow.className = "dash-row";
            const customInput = document.createElement("input");
            customInput.type = "text";
            customInput.className = "dash-input";
            customInput.style.flex = "1";
            customInput.placeholder = "Custom IANA zone (e.g. America/Chicago)";
            const customAdd = document.createElement("button");
            customAdd.type = "button";
            customAdd.className = "dash-btn";
            customAdd.textContent = "Add";
            const errEl = document.createElement("p");
            errEl.className = "form-error hidden";
            customAdd.addEventListener("click", () => {
                const tz = customInput.value.trim();
                if (!isValidTimeZone(tz)) {
                    errEl.textContent = "Unknown IANA time zone identifier.";
                    errEl.classList.remove("hidden");
                    return;
                }
                errEl.classList.add("hidden");
                draftZones.push({ label: tz.split("/").pop().replace(/_/g, " "), tz });
                customInput.value = "";
                renderDraft();
            });
            customRow.append(customInput, customAdd);

            const footer = document.createElement("div");
            footer.className = "dialog-footer";
            const saveBtn = document.createElement("button");
            saveBtn.type = "button";
            saveBtn.className = "primary-button";
            saveBtn.textContent = "Save";
            saveBtn.addEventListener("click", async () => {
                await api.updateConfig({ zones: draftZones, use24Hour: draft24Hour, showSeconds: draftSeconds });
                close();
            });
            footer.append(saveBtn);

            container.append(hourSwitch, secondsSwitch, zonesLabel, list, addRow, customRow, errEl, footer);
        },
        unmount() {
            clearTimeout(timerId);
            document.removeEventListener("visibilitychange", onVisibility);
        },
    };
}
