/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #23 Weather (weather)
 * Manual coordinates or local preset only (no geolocation, no city autocomplete, fixed Open-Meteo adapter).
 * Displays provider & approximate coordinates before consent. Converts units locally.
 */

import { icon } from "../icons.js";

const LOCAL_PRESETS = [
    { label: "New York", lat: 40.71, lon: -74.01 },
    { label: "San Francisco", lat: 37.77, lon: -122.42 },
    { label: "London", lat: 51.51, lon: -0.13 },
    { label: "Berlin", lat: 52.52, lon: 13.40 },
    { label: "Tokyo", lat: 35.68, lon: 139.69 },
    { label: "Sydney", lat: -33.87, lon: 151.21 },
];

function weatherCodeLabel(code) {
    if (code === 0) return "Clear";
    if (code <= 3) return "Partly cloudy";
    if (code <= 48) return "Fog";
    if (code <= 57) return "Drizzle";
    if (code <= 67) return "Rain";
    if (code <= 77) return "Snow";
    if (code <= 82) return "Showers";
    if (code >= 95) return "Thunderstorm";
    return "Cloudy";
}

function fmtTemp(tempC, unit) {
    if (typeof tempC !== "number" || !Number.isFinite(tempC))
        return "—";
    if (unit === "F")
        return `${Math.round((tempC * 9) / 5 + 32)}°F`;
    return `${Math.round(tempC)}°C`;
}

// The figure shows the number and degree sign; the unit is set in Settings and shown small.
function tempNumber(tempC, unit) {
    if (typeof tempC !== "number" || !Number.isFinite(tempC))
        return "—";
    return `${Math.round(unit === "F" ? (tempC * 9) / 5 + 32 : tempC)}°`;
}

// Only three weather glyphs exist in icons.js: sun, moon and a cloud.
function conditionGlyph(code) {
    return code === 0 ? "sun" : "weather";
}

// "Now" at the forecast's location, as the "YYYY-MM-DDTHH" prefix Open-Meteo uses for hours.
function localHourKey(timeZone) {
    try {
        const parts = new Intl.DateTimeFormat("en-CA", {
            timeZone: timeZone || "UTC",
            year: "numeric",
            month: "2-digit",
            day: "2-digit",
            hour: "2-digit",
            hourCycle: "h23",
        }).formatToParts(new Date());
        const get = type => parts.find(p => p.type === type)?.value || "";
        return `${get("year")}-${get("month")}-${get("day")}T${get("hour")}`;
    } catch {
        return "";
    }
}

function dayName(isoDate, today) {
    if (today && isoDate === today)
        return "Today";
    const d = new Date(`${isoDate}T12:00:00Z`);
    if (!Number.isFinite(d.getTime()))
        return (isoDate || "").slice(5);
    return d.toLocaleDateString([], { weekday: "short", timeZone: "UTC" });
}

export function mount(root, api) {
    let config = { sourceId: "", unit: "C", ...api.config };
    let tier = api.tier;
    let state = { allowed: false, offline: false, grants: [] };
    let statusMsg = "";
    let autoTimer = null;

    root.classList.add("wx");

    const wrap = document.createElement("div");
    wrap.className = "wx-face";
    root.append(wrap);

    function activeGrant() {
        const list = (state.grants || []).filter(g => g.kind === "weather");
        if (config.sourceId) {
            const exact = list.find(g => g.sourceId === config.sourceId);
            if (exact)
                return exact;
        }
        return list[0] || null;
    }

    // The consent form. It is drawn in the tile and in the Settings dialog, so it is styled
    // by its own classes and does not depend on the tile around it.
    function renderSetup(container, onDone = null) {
        container.replaceChildren();
        container.classList.add("net-consent");

        const explain = document.createElement("div");
        explain.className = "net-consent-text";
        const glyph = document.createElement("span");
        glyph.className = "net-consent-glyph";
        glyph.append(icon("weather", 16));
        const words = document.createElement("div");
        words.className = "net-consent-words";
        const disclosure = document.createElement("p");
        disclosure.textContent = "Weather uses Open-Meteo (api.open-meteo.com) with coordinates rounded to ~1 km (2 decimals). No geolocation or city lookup is performed.";
        // Repeats, as the fields change, exactly which coordinates would be sent.
        const sends = document.createElement("p");
        sends.className = "net-consent-sends";
        sends.setAttribute("aria-live", "polite");
        words.append(disclosure, sends);
        explain.append(glyph, words);

        const presetSel = document.createElement("select");
        presetSel.className = "dash-select net-consent-field";
        presetSel.setAttribute("aria-label", "Location preset");
        const customOpt = document.createElement("option");
        customOpt.value = "";
        customOpt.textContent = "Choose a local city preset or enter coordinates…";
        presetSel.append(customOpt);
        for (const p of LOCAL_PRESETS) {
            const opt = document.createElement("option");
            opt.value = `${p.label}|${p.lat}|${p.lon}`;
            opt.textContent = `${p.label} (${p.lat.toFixed(2)}, ${p.lon.toFixed(2)})`;
            presetSel.append(opt);
        }

        const labelInp = document.createElement("input");
        labelInp.type = "text";
        labelInp.className = "dash-input net-consent-field";
        labelInp.placeholder = "Location label";
        labelInp.setAttribute("aria-label", "Location label");

        const coordRow = document.createElement("div");
        coordRow.className = "form-grid-2 net-consent-coords";
        const latInp = document.createElement("input");
        latInp.type = "number";
        latInp.step = "0.01";
        latInp.className = "dash-input";
        latInp.placeholder = "Latitude (-90..90)";
        latInp.setAttribute("aria-label", "Latitude");
        const lonInp = document.createElement("input");
        lonInp.type = "number";
        lonInp.step = "0.01";
        lonInp.className = "dash-input";
        lonInp.placeholder = "Longitude (-180..180)";
        lonInp.setAttribute("aria-label", "Longitude");
        coordRow.append(latInp, lonInp);

        const updateSends = () => {
            const lat = Number(latInp.value);
            const lon = Number(lonInp.value);
            // Shown only for coordinates the browser would accept, rounded the way it rounds them.
            const ok = latInp.value !== "" && lonInp.value !== "" && Number.isFinite(lat) && Number.isFinite(lon)
                && lat >= -90 && lat <= 90 && lon >= -180 && lon <= 180;
            sends.replaceChildren();
            sends.classList.toggle("hidden", !ok);
            if (!ok)
                return;
            const coords = document.createElement("strong");
            const round2 = v => (Math.sign(v) * Math.round(Math.abs(v) * 100) / 100).toFixed(2);
            coords.textContent = `${round2(lat)}, ${round2(lon)}`;
            const host = document.createElement("strong");
            host.textContent = "api.open-meteo.com";
            sends.append("Sends ", coords, " to ", host, ".");
        };
        updateSends();
        latInp.addEventListener("input", updateSends);
        lonInp.addEventListener("input", updateSends);

        presetSel.addEventListener("change", () => {
            if (!presetSel.value)
                return;
            const [lbl, lat, lon] = presetSel.value.split("|");
            labelInp.value = lbl;
            latInp.value = lat;
            lonInp.value = lon;
            updateSends();
        });

        const autoLbl = document.createElement("label");
        autoLbl.className = "net-consent-check";
        const autoCb = document.createElement("input");
        autoCb.type = "checkbox";
        const autoTxt = document.createElement("span");
        autoTxt.textContent = "Auto-refresh while visible (30 min)";
        autoLbl.append(autoCb, autoTxt);

        const errEl = document.createElement("span");
        errEl.className = "form-error net-consent-error hidden";

        const confirmBtn = document.createElement("button");
        confirmBtn.type = "button";
        confirmBtn.className = "dash-btn primary net-consent-confirm";
        confirmBtn.textContent = "Consent & save location";
        confirmBtn.addEventListener("click", async () => {
            errEl.classList.add("hidden");
            const lat = Number(latInp.value);
            const lon = Number(lonInp.value);
            const label = labelInp.value.trim() || "Weather";
            try {
                // Switching location revokes old grant if this card owned one
                if (config.sourceId)
                    await api.request("network.revoke", { sourceId: config.sourceId });
                const res = await api.request("network.grant", {
                    kind: "weather",
                    lat,
                    lon,
                    label,
                    autoRefresh: autoCb.checked,
                });
                config.sourceId = res.sourceId;
                await api.updateConfig({ sourceId: res.sourceId });
                await load();
                await refresh(res.sourceId);
                onDone?.();
            } catch (err) {
                errEl.textContent = err.message || "Invalid coordinates.";
                errEl.classList.remove("hidden");
            }
        });

        container.append(explain, presetSel, labelInp, coordRow, autoLbl, errEl, confirmBtn);
    }

    function smallButton(glyph, label, onClick) {
        const b = document.createElement("button");
        b.type = "button";
        b.className = "dash-btn ghost icon small";
        b.setAttribute("aria-label", label);
        b.title = label;
        b.append(icon(glyph, 14));
        b.addEventListener("click", onClick);
        return b;
    }

    function render() {
        wrap.replaceChildren();
        wrap.className = "wx-face";
        if (!state.allowed) {
            api.setMeta("Disabled");
            const empty = document.createElement("div");
            empty.className = "dash-empty-card";
            empty.textContent = "Weather is disabled in private or temporary sessions.";
            wrap.append(empty);
            return;
        }

        const grant = activeGrant();
        if (!grant) {
            api.setMeta("Not configured");
            if (tier === "compact") {
                // Too small for the consent form; it is in Settings, with nothing sent until then.
                wrap.classList.add("is-unset");
                const glyph = document.createElement("span");
                glyph.className = "wx-unset-glyph";
                glyph.append(icon("weather", 16));
                const msg = document.createElement("span");
                msg.className = "wx-unset-text";
                msg.textContent = "Choose a location in Settings…";
                wrap.append(glyph, msg);
                return;
            }
            const setup = document.createElement("div");
            setup.className = "wx-setup";
            renderSetup(setup);
            wrap.append(setup);
            return;
        }

        const cache = grant.cache || {};
        const cur = cache.current;
        const locLabel = grant.config?.label || "Weather";
        api.setMeta(state.offline ? `${locLabel} · Offline` : locLabel);

        const unitBtn = document.createElement("button");
        unitBtn.type = "button";
        unitBtn.className = "dash-btn ghost small wx-unit";
        unitBtn.textContent = config.unit === "F" ? "°F" : "°C";
        unitBtn.setAttribute("aria-label", `Temperature unit: ${config.unit === "F" ? "Fahrenheit" : "Celsius"}. Switch unit`);
        unitBtn.title = "Switch unit";
        unitBtn.addEventListener("click", () => {
            config.unit = config.unit === "F" ? "C" : "F";
            api.updateConfig({ unit: config.unit });
            render();
        });
        const refBtn = smallButton("refresh", "Refresh", () => refresh(grant.sourceId));

        // The hero: a big thin temperature with the condition beside or under it.
        const now = document.createElement("div");
        now.className = "wx-now";
        const fig = document.createElement("span");
        fig.className = "dash-figure wx-temp";
        fig.textContent = cur ? tempNumber(cur.tempC, config.unit) : "—";
        if (cur && tier === "compact") {
            const unit = document.createElement("span");
            unit.className = "dash-figure-unit wx-temp-unit";
            unit.textContent = config.unit === "F" ? "F" : "C";
            fig.append(unit);
        }
        const cond = document.createElement("div");
        cond.className = "wx-cond";
        if (cur) {
            const condLine = document.createElement("span");
            condLine.className = "wx-cond-line";
            const g = document.createElement("span");
            g.className = "wx-cond-glyph";
            g.append(icon(conditionGlyph(cur.weatherCode), tier === "compact" ? 14 : 16));
            const t = document.createElement("span");
            t.className = "dash-truncate";
            t.textContent = weatherCodeLabel(cur.weatherCode);
            condLine.append(g, t);
            cond.append(condLine);
            if (tier !== "compact") {
                const detail = document.createElement("span");
                detail.className = "dash-truncate wx-detail";
                detail.textContent = `Feels like ${fmtTemp(cur.feelsLikeC, config.unit)} · Humidity ${cur.humidity ?? "—"}%`;
                cond.append(detail);
            }
        } else {
            const none = document.createElement("span");
            none.className = "dash-truncate wx-detail";
            none.textContent = tier === "compact" ? "No data" : "No forecast yet";
            cond.append(none);
        }

        const tools = document.createElement("div");
        tools.className = "wx-tools";
        tools.append(unitBtn, refBtn);

        if (tier === "compact") {
            wrap.classList.add("is-compact");
            now.append(fig, cond);
            // Compact keeps Refresh; the unit lives in Settings and on the larger sizes.
            wrap.append(now, refBtn);
            if (statusMsg || state.offline) {
                // One line in place of the condition; the full message is in the tooltip.
                const st = document.createElement("span");
                st.className = "dash-truncate wx-detail";
                st.textContent = state.offline ? "Offline" : statusMsg;
                st.title = st.textContent;
                cond.replaceChildren(st);
            }
            return;
        }

        const main = document.createElement("div");
        main.className = "wx-main";
        const left = document.createElement("div");
        left.className = "wx-left";
        now.append(fig);
        left.append(now, cond);

        if (statusMsg || state.offline) {
            const st = document.createElement("div");
            st.className = "dash-truncate wx-status";
            st.textContent = state.offline ? "Offline mode — showing last forecast." : statusMsg;
            st.title = st.textContent;
            left.append(st);
        }
        main.append(left);

        if (tier === "expanded") {
            const strips = document.createElement("div");
            strips.className = "wx-strips";
            const hourly = Array.isArray(cache.hourly) ? cache.hourly : [];
            if (hourly.length > 0) {
                const key = localHourKey(cache.timezone);
                let start = key ? hourly.findIndex(h => (h.time || "").slice(0, 13) >= key) : 0;
                if (start < 0)
                    start = 0;
                const strip = document.createElement("div");
                strip.className = "wx-strip";
                strip.setAttribute("aria-label", "Next hours");
                hourly.slice(start, start + 6).forEach((h, i) => {
                    const cell = document.createElement("div");
                    cell.className = "wx-cell";
                    const when = document.createElement("span");
                    when.className = "wx-cell-label";
                    when.textContent = i === 0 && start > 0 ? "Now" : (h.time || "").slice(11, 16);
                    const gl = document.createElement("span");
                    gl.className = "wx-cell-glyph";
                    gl.append(icon(conditionGlyph(h.weatherCode), 14));
                    const tv = document.createElement("span");
                    tv.className = "wx-cell-value tabular";
                    tv.textContent = tempNumber(h.tempC, config.unit);
                    cell.append(when, gl, tv);
                    strip.append(cell);
                });
                strips.append(strip);
            }
            if (Array.isArray(cache.daily) && cache.daily.length > 0) {
                const strip = document.createElement("div");
                strip.className = "wx-strip";
                strip.setAttribute("aria-label", "Next days");
                cache.daily.slice(0, 5).forEach(d => {
                    const cell = document.createElement("div");
                    cell.className = "wx-cell";
                    const day = document.createElement("span");
                    day.className = "wx-cell-label";
                    day.textContent = dayName(d.date, localHourKey(cache.timezone).slice(0, 10));
                    const gl = document.createElement("span");
                    gl.className = "wx-cell-glyph";
                    gl.append(icon(conditionGlyph(d.weatherCode), 14));
                    const hiLo = document.createElement("span");
                    hiLo.className = "wx-cell-value tabular";
                    const hi = document.createElement("span");
                    hi.textContent = tempNumber(d.maxC, config.unit);
                    const lo = document.createElement("span");
                    lo.className = "wx-lo";
                    lo.textContent = tempNumber(d.minC, config.unit);
                    hiLo.append(hi, " ", lo);
                    hiLo.setAttribute("aria-label", `High ${fmtTemp(d.maxC, config.unit)}, low ${fmtTemp(d.minC, config.unit)}`);
                    cell.append(day, gl, hiLo);
                    strip.append(cell);
                });
                strips.append(strip);
            }
            if (strips.childElementCount)
                main.append(strips);
        }

        const foot = document.createElement("div");
        foot.className = "wx-foot";
        const attr = document.createElement("span");
        attr.className = "wx-attr";
        const ts = grant.lastSuccessAt > 0
            ? `Updated ${new Date(grant.lastSuccessAt).toLocaleTimeString([], { hour: "2-digit", minute: "2-digit" })} · `
            : "";
        attr.textContent = `${ts}${cache.attribution || "Open-Meteo.com"}`;
        attr.title = attr.textContent;
        foot.append(attr, tools);

        wrap.append(main, foot);
    }

    async function refresh(sourceId) {
        statusMsg = "Fetching forecast…";
        render();
        try {
            await api.request("network.refresh", { sourceId });
            statusMsg = "";
            await load();
        } catch (err) {
            statusMsg = err.message || "Forecast unavailable (preserving last data).";
            render();
        }
    }

    function scheduleAutoRefresh() {
        clearInterval(autoTimer);
        const grant = activeGrant();
        if (!grant?.config?.autoRefresh || document.hidden || state.offline)
            return;
        autoTimer = setInterval(() => {
            if (!document.hidden && !state.offline)
                refresh(grant.sourceId);
        }, 30 * 60 * 1000);
    }

    async function load() {
        state = await api.request("network.listGrants", {});
        render();
        scheduleAutoRefresh();
    }

    const offGrants = api.onEvent("networkGrantsChanged", () => load());
    const offPrivacy = api.onEvent("privacyChanged", () => load());
    const onVis = () => {
        if (document.hidden)
            clearInterval(autoTimer);
        else
            scheduleAutoRefresh();
    };
    document.addEventListener("visibilitychange", onVis);

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            config = { sourceId: "", unit: "C", ...next.config };
            tier = next.tier;
            render();
        },
        renderConfig(container, close) {
            const grant = activeGrant();
            if (grant) {
                const curRow = document.createElement("div");
                curRow.className = "dash-row-between";
                const lbl = document.createElement("span");
                lbl.textContent = `Current: ${grant.config?.label} (${grant.config?.lat}, ${grant.config?.lon})`;
                const rev = document.createElement("button");
                rev.type = "button";
                rev.className = "dash-btn ghost small";
                rev.textContent = "Revoke & clear cache";
                rev.addEventListener("click", async () => {
                    await api.request("network.revoke", { sourceId: grant.sourceId });
                    config.sourceId = "";
                    await api.updateConfig({ sourceId: "" });
                    await load();
                    close();
                });
                curRow.append(lbl, rev);
                container.append(curRow);
            }
            if (grant) {
                // The unit, also reachable here because the smallest tile has no room for it.
                const unitRow = document.createElement("div");
                unitRow.className = "dash-row-between";
                const unitLbl = document.createElement("span");
                unitLbl.textContent = "Temperature unit";
                const seg = document.createElement("div");
                seg.className = "dash-segments";
                seg.setAttribute("role", "group");
                seg.setAttribute("aria-label", "Temperature unit");
                for (const [val, lbl] of [["C", "°C"], ["F", "°F"]]) {
                    const b = document.createElement("button");
                    b.type = "button";
                    b.className = "dash-btn";
                    b.textContent = lbl;
                    b.setAttribute("aria-pressed", String(config.unit === val));
                    b.addEventListener("click", async () => {
                        config.unit = val;
                        for (const other of seg.children)
                            other.setAttribute("aria-pressed", String(other === b));
                        await api.updateConfig({ unit: val });
                        render();
                    });
                    seg.append(b);
                }
                unitRow.append(unitLbl, seg);
                container.append(unitRow);
            }
            const formWrap = document.createElement("div");
            formWrap.className = "dash-col";
            renderSetup(formWrap, close);
            container.append(formWrap);
        },
        unmount() {
            clearInterval(autoTimer);
            offGrants();
            offPrivacy();
            document.removeEventListener("visibilitychange", onVis);
        },
    };
}
