/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Line glyphs for about:dashboard, drawn on a 24 px grid.
 * Built with DOM calls so the page needs no inline markup or image loads.
 */

import { onDashboardEvent, request } from "./bridge.js";

const SVG_NS = "http://www.w3.org/2000/svg";

// Each glyph is a list of shapes. A string is a stroked path; an object is
// { d, fill: true } for a filled path or { c: [cx, cy, r], fill? } for a circle.
const GLYPHS = {
    // Widgets
    "top-sites": ["M12 3.8l2.5 5.2 5.7.8-4.1 4 1 5.7-5.1-2.7-5.1 2.7 1-5.7-4.1-4 5.7-.8z"],
    "search-box": [{ c: [11, 11, 6.5] }, "M16 16l4.2 4.2"],
    "open-tabs": ["M4 8h16v10.5a1.5 1.5 0 0 1-1.5 1.5h-13A1.5 1.5 0 0 1 4 18.500z", "M4 8V6a1.5 1.5 0 0 1 1.5-1.5h4.300a1.500 1.500 0 0 1 1.300.800L12 8"],
    "recently-closed": ["M4.5 12a7.5 7.5 0 1 0 2.300-5.400", "M4.300 4.500v3.700H8", "M12 8.200v4l2.700 1.700"],
    "workspaces": ["M12 4l8 4.200-8 4.200-8-4.200z", "M4.500 12.200l7.500 4 7.500-4", "M4.500 16l7.500 4 7.500-4"],
    "bookmark-shelf": ["M7.500 4.500h9a1 1 0 0 1 1 1V20L12 16.200 6.500 20V5.500a1 1 0 0 1 1-1z"],
    "recent-history": [{ c: [12, 12, 8.5] }, "M12 7.500V12l3 2"],
    "downloads": ["M12 4v10.500", "M7.500 10.500l4.500 4.500 4.500-4.500", "M5 19.500h14"],
    "reading-queue": ["M12 6.500C10.200 5.200 7.700 4.700 4.500 5v13c3.200-.300 5.700.200 7.500 1.500 1.800-1.300 4.300-1.800 7.500-1.500V5c-3.200-.300-5.700.200-7.500 1.500z", "M12 6.500v13"],
    "feed-reader": ["M5 11a8 8 0 0 1 8 8", "M5 5a14 14 0 0 1 14 14", { c: [6.2, 17.8, 1.4], fill: true }],
    "privacy-summary": ["M12 3.500l7 2.600v5.400c0 4.300-2.800 7.600-7 9-4.200-1.400-7-4.700-7-9V6.100z", "M9 12l2.200 2.200 4-4.400"],
    "extensions-dock": ["M4.500 4.500h6v6h-6z", "M4.500 13.500h6v6h-6z", "M13.500 13.500h6v6h-6z", "M16.500 3.300l4.200 4.200-4.200 4.200-4.200-4.200z"],
    "permissions-audit": ["M4 8h8.500", "M17.500 8H20", "M4 16h2.500", "M11.500 16H20", { c: [15, 8, 2.3] }, { c: [9, 16, 2.3] }],
    "profile-switcher": [{ c: [12, 8.5, 3.6] }, "M5 19.500c.800-3.300 3.500-5 7-5s6.200 1.700 7 5"],
    "browser-health": ["M3.500 12.500h3.800l2.200-5.500 4 10.500 2.300-5h4.700"],
    "world-clock": [{ c: [12, 12, 8.5] }, "M3.500 12h17", "M12 3.500c2.400 2.600 3.600 5.400 3.600 8.500s-1.200 5.900-3.600 8.500c-2.400-2.600-3.600-5.400-3.600-8.500S9.600 6.100 12 3.500z"],
    "focus-timer": [{ c: [12, 13.2, 7] }, "M12 9.700v3.500", "M10 3.500h4", "M12 3.500v2.700"],
    "quick-tasks": [{ c: [12, 12, 8.5] }, "M8.300 12.300l2.500 2.500 4.900-5.300"],
    "scratchpad": ["M5.500 4.500h9l4 4v11h-13z","M8.500 10.500h7", "M8.500 13.800h7", "M8.500 17h4"],
    "calendar-countdown": ["M4.500 7A1.500 1.500 0 0 1 6 5.500h12A1.500 1.500 0 0 1 19.500 7v11a1.500 1.500 0 0 1-1.500 1.500H6A1.500 1.500 0 0 1 4.500 18z", "M4.500 10h15", "M8.500 3.500V7", "M15.500 3.500V7"],
    "habit-tracker": ["M4.500 12a7.500 7.500 0 0 1 12.700-5.400", "M19.500 12a7.500 7.500 0 0 1-12.700 5.400", "M17.500 3.300v3.500H14", "M6.500 20.700v-3.500H10"],
    "now-playing": ["M9 17.500V6.200l10-2v11.300", { c: [6.7, 17.5, 2.3] }, { c: [16.7, 15.5, 2.3] }],
    "weather": ["M7.500 19a3.600 3.600 0 0 1-.400-7.200 5 5 0 0 1 9.700 1.100 3.100 3.100 0 0 1-.300 6.100z", "M17 6.500a2.800 2.800 0 0 1 2.300 4.400"],
    "calculator": ["M6.500 3.500h11A1.500 1.500 0 0 1 19 5v14a1.500 1.500 0 0 1-1.500 1.500h-11A1.500 1.500 0 0 1 5 19V5a1.500 1.500 0 0 1 1.500-1.500z", "M8.500 7.800h7", { c: [8.7, 12.2, 0.6], fill: true }, { c: [12, 12.2, 0.6], fill: true }, { c: [15.3, 12.2, 0.6], fill: true }, { c: [8.7, 16, 0.6], fill: true }, { c: [12, 16, 0.6], fill: true }, { c: [15.3, 16, 0.6], fill: true }],
    "color-contrast": [{ c: [12, 12, 8.5] }, { d: "M12 3.500a8.500 8.500 0 0 1 0 17z", fill: true }],
    "dev-codec": ["M9.500 4.500L8 19.500", "M16 4.500l-1.500 15", "M4.500 9h15.500", "M4 15h15.500"],
    "snippets": ["M8.500 7.500L4 12l4.500 4.500", "M15.500 7.500L20 12l-4.500 4.500", "M13.500 5l-3 14"],
    "url-inspector": ["M10 14a4 4 0 0 0 5.700 0l3-3a4 4 0 0 0-5.700-5.700l-1 1", "M14 10a4 4 0 0 0-5.700 0l-3 3a4 4 0 0 0 5.700 5.700l1-1"],
    "shortcuts-guide": ["M3.500 7.500A1.500 1.500 0 0 1 5 6h14a1.500 1.500 0 0 1 1.500 1.500v9A1.500 1.500 0 0 1 19 18H5a1.500 1.500 0 0 1-1.500-1.500z", "M8 14.500h8", { c: [7, 10, 0.6], fill: true }, { c: [10.3, 10, 0.6], fill: true }, { c: [13.7, 10, 0.6], fill: true }, { c: [17, 10, 0.6], fill: true }],
    "pinned-references": ["M9 4.500h6l-1 5 3.200 3.500H6.800L10 9.500z", "M12 13v7"],

    "image": ["M5 5h14v14H5z", { c: [9.5, 9.5, 1.4] }, "M5 16l4-3 3 2 3-4 4 5"],

    // Interface
    "plus": ["M12 5.500v13", "M5.500 12h13"],
    "minus": ["M6.500 12h11"],
    "close": ["M6.500 6.500l11 11", "M17.500 6.500l-11 11"],
    "check": ["M5.500 12.500l4.200 4.200 8.800-9.400"],
    "more": [{ c: [6, 12, 1.5], fill: true }, { c: [12, 12, 1.5], fill: true }, { c: [18, 12, 1.5], fill: true }],
    "undo": ["M8 5.500L4 9.500l4 4", "M4.500 9.500h9a5 5 0 0 1 0 10H10"],
    "redo": ["M16 5.500l4 4-4 4", "M19.500 9.500h-9a5 5 0 0 0 0 10H14"],
    "presets": ["M4.500 6A1.500 1.500 0 0 1 6 4.500h5.500V12h-7z", "M14.500 4.500H18A1.500 1.500 0 0 1 19.500 6v2.500h-5z", "M14.500 11.500h5V18a1.500 1.500 0 0 1-1.500 1.500h-3.500z", "M4.500 15h7v4.500H6A1.500 1.500 0 0 1 4.500 18z"],
    "tidy": ["M5 4.500h14", "M12 20V9", "M7.500 13.300L12 8.800l4.500 4.500"],
    "customize": ["M4.500 19.500l1-4.200L16 4.800a1.800 1.800 0 0 1 2.500 0l.700.700a1.800 1.800 0 0 1 0 2.500L8.700 18.500z", "M14 6.800l3.200 3.200"],
    "swap": ["M7 4.500v15", "M3.500 8L7 4.500 10.500 8", "M17 19.500v-15", "M13.500 16l3.500 3.500 3.500-3.500"],
    "replace": ["M4.500 10a7.500 7.500 0 0 1 13.200-3.700l1.800 2", "M19.500 4.300v4h-4", "M19.500 14a7.500 7.500 0 0 1-13.200 3.700l-1.800-2", "M4.500 19.700v-4h4"],
    "configure": ["M4 8h8.500", "M17.500 8H20", "M4 16h2.500", "M11.500 16H20", { c: [15, 8, 2.3] }, { c: [9, 16, 2.3] }],
    "trash": ["M5 7h14", "M9.500 7V4.500h5V7", "M7 7l.800 12a1 1 0 0 0 1 1h6.400a1 1 0 0 0 1-1L17 7"],
    "private": ["M7.500 10.500V8a4.500 4.500 0 0 1 9 0v2.500", "M6 10.500h12v9H6z"],
    "search": [{ c: [11, 11, 6.5] }, "M16 16l4.200 4.200"],
    "arrow-up": ["M12 19V5.500", "M6.500 11L12 5.500 17.500 11"],
    "arrow-down": ["M12 5v13.500", "M6.500 13L12 18.500 17.500 13"],
    "play": [{ d: "M8 5.500v13l10.500-6.500z", fill: true }],
    "pause": [{ d: "M7.500 5.500h3v13h-3z", fill: true }, { d: "M13.500 5.500h3v13h-3z", fill: true }],
    "reset": ["M5 12a7 7 0 1 0 2.200-5.100", "M4.800 4.800v3.700h3.700"],
    "external": ["M13.500 5H19v5.500", "M19 5l-8 8", "M17 13.500V18a1 1 0 0 1-1 1H6a1 1 0 0 1-1-1V8a1 1 0 0 1 1-1h4.500"],
    "copy": ["M9 9h9.500a1 1 0 0 1 1 1v9.500a1 1 0 0 1-1 1H9a1 1 0 0 1-1-1V10a1 1 0 0 1 1-1z", "M5 15V5a1 1 0 0 1 1-1h10"],
    "refresh": ["M19 12a7 7 0 1 1-2.200-5.100", "M19.200 4.800v3.700h-3.700"],
    "eye": ["M2.800 12c2.200-4 5.300-6 9.200-6s7 2 9.200 6c-2.200 4-5.300 6-9.200 6s-7-2-9.200-6z", { c: [12, 12, 2.8] }],
    "pin": ["M9 4.500h6l-1 5 3.200 3.500H6.800L10 9.500z", "M12 13v7"],
    "lock": ["M6.500 10.500h11a1 1 0 0 1 1 1v7a1 1 0 0 1-1 1h-11a1 1 0 0 1-1-1v-7a1 1 0 0 1 1-1z", "M8.500 10.500V8a3.500 3.500 0 0 1 7 0v2.500", { c: [12, 15, 1.1], fill: true }],
    "unlock": ["M6.500 10.500h11a1 1 0 0 1 1 1v7a1 1 0 0 1-1 1h-11a1 1 0 0 1-1-1v-7a1 1 0 0 1 1-1z", "M8.500 10.500V8a3.500 3.500 0 0 1 6.700-1.400", { c: [12, 15, 1.1], fill: true }],
    "mute": ["M4.500 9.500h3l4.500-4v13l-4.500-4h-3z", "M16 9.500l4 5", "M20 9.500l-4 5"],
    "sound": ["M4.500 9.500h3l4.500-4v13l-4.500-4h-3z", "M15.500 9a4.300 4.300 0 0 1 0 6", "M18 6.500a7.800 7.800 0 0 1 0 11"],
    "prev": [{ d: "M19 6v12l-9-6z", fill: true }, "M6 6v12"],
    "next": [{ d: "M5 6v12l9-6z", fill: true }, "M18 6v12"],
    "folder": ["M3.500 7A1.500 1.500 0 0 1 5 5.500h4.200l2 2.300H19a1.500 1.500 0 0 1 1.500 1.500V17a1.500 1.500 0 0 1-1.500 1.500H5A1.500 1.500 0 0 1 3.500 17z"],
    "arrow-right": ["M5 12h13.500", "M13 6.500l5.500 5.500-5.500 5.500"],
    "chevron-right": ["M9.500 6l6 6-6 6"],
    "chevron-left": ["M14.500 6l-6 6 6 6"],
    "chevron-down": ["M6 9.500l6 6 6-6"],
    "info": [{ c: [12, 12, 8.5] }, "M12 11v5.500", { c: [12, 7.8, 0.7], fill: true }],
    "warning": ["M12 4.500l8.500 14.500h-17z", "M12 10v4", { c: [12, 16.4, 0.7], fill: true }],
    "globe": [{ c: [12, 12, 8.5] }, "M3.500 12h17", "M12 3.500c2.400 2.600 3.600 5.400 3.600 8.500s-1.200 5.900-3.600 8.500c-2.400-2.600-3.600-5.400-3.600-8.500S9.600 6.100 12 3.500z"],
    "stop": [{ d: "M7 7h10v10H7z", fill: true }],
    "sun": [{ c: [12, 12, 4] }, "M12 3v2", "M12 19v2", "M3 12h2", "M19 12h2", "M5.600 5.600l1.400 1.400", "M17 17l1.400 1.400", "M5.600 18.400L7 17", "M17 7l1.400-1.400"],
    "moon": ["M19.500 14.500A8 8 0 0 1 9.500 4.500a8 8 0 1 0 10 10z"],
};

/**
 * Returns an inline SVG element for the named glyph. `size` is in CSS pixels.
 * The glyph strokes with `currentColor`.
 */
export function icon(name, size = 16) {
    const shapes = GLYPHS[name] || GLYPHS["more"];
    const svg = document.createElementNS(SVG_NS, "svg");
    svg.setAttribute("viewBox", "0 0 24 24");
    svg.setAttribute("width", String(size));
    svg.setAttribute("height", String(size));
    svg.setAttribute("fill", "none");
    svg.setAttribute("stroke", "currentColor");
    svg.setAttribute("stroke-width", "1.75");
    svg.setAttribute("stroke-linecap", "round");
    svg.setAttribute("stroke-linejoin", "round");
    svg.setAttribute("aria-hidden", "true");
    svg.setAttribute("focusable", "false");
    svg.classList.add("dash-icon");

    for (const shape of shapes) {
        let el;
        if (typeof shape === "string") {
            el = document.createElementNS(SVG_NS, "path");
            el.setAttribute("d", shape);
        } else if (shape.c) {
            el = document.createElementNS(SVG_NS, "circle");
            el.setAttribute("cx", String(shape.c[0]));
            el.setAttribute("cy", String(shape.c[1]));
            el.setAttribute("r", String(shape.c[2]));
        } else {
            el = document.createElementNS(SVG_NS, "path");
            el.setAttribute("d", shape.d);
        }
        if (typeof shape === "object" && shape.fill) {
            el.setAttribute("fill", "currentColor");
            el.setAttribute("stroke", "none");
        }
        svg.append(el);
    }
    return svg;
}

export function hasIcon(name) {
    return Object.hasOwn(GLYPHS, name);
}

/**
 * A progress ring. `fraction` is 0..1. Returns { el, set(fraction) }.
 * The track uses the muted control color and the arc uses `currentColor`.
 */
export function ring(size = 64, stroke = 6) {
    const r = (size - stroke) / 2;
    const circumference = 2 * Math.PI * r;
    const svg = document.createElementNS(SVG_NS, "svg");
    svg.setAttribute("viewBox", `0 0 ${size} ${size}`);
    svg.setAttribute("width", String(size));
    svg.setAttribute("height", String(size));
    svg.setAttribute("aria-hidden", "true");
    svg.classList.add("dash-ring");

    const track = document.createElementNS(SVG_NS, "circle");
    const arc = document.createElementNS(SVG_NS, "circle");
    for (const c of [track, arc]) {
        c.setAttribute("cx", String(size / 2));
        c.setAttribute("cy", String(size / 2));
        c.setAttribute("r", String(r));
        c.setAttribute("fill", "none");
        c.setAttribute("stroke-width", String(stroke));
    }
    track.classList.add("dash-ring-track");
    arc.classList.add("dash-ring-arc");
    arc.setAttribute("stroke", "currentColor");
    arc.setAttribute("stroke-linecap", "round");
    arc.setAttribute("stroke-dasharray", String(circumference));
    arc.setAttribute("transform", `rotate(-90 ${size / 2} ${size / 2})`);
    svg.append(track, arc);

    const set = fraction => {
        const f = Math.max(0, Math.min(1, Number.isFinite(fraction) ? fraction : 0));
        arc.setAttribute("stroke-dashoffset", String(circumference * (1 - f)));
        arc.setAttribute("visibility", f <= 0 ? "hidden" : "visible");
    };
    set(0);
    return { el: svg, set };
}

// --- Site logos -----------------------------------------------------------------------
//
// A site is pictured by its favicon wherever the browser already knows one: from the
// item itself, or else from a lookup against this board's own history and open tabs.
// Nothing is fetched from the network. Until a logo arrives, and whenever there is none
// or it fails to decode, the site keeps its fallback: its initial, or a glyph.

const LOOKUP_BATCH = 48;
const faviconCache = new Map(); // url -> base64 PNG, or "" when the browser knows none
const waiting = new Map(); // url -> [resolve]
let flushScheduled = false;

onDashboardEvent("historyCleared", () => faviconCache.clear());

async function flushLookups() {
    flushScheduled = false;
    const urls = [...waiting.keys()];
    for (let i = 0; i < urls.length; i += LOOKUP_BATCH) {
        const batch = urls.slice(i, i + LOOKUP_BATCH);
        let icons = {};
        try {
            icons = (await request("favicons.lookup", { urls: batch })).icons || {};
        } catch {
            // Without the host, every site keeps its fallback.
        }
        for (const url of batch) {
            const value = typeof icons[url] === "string" ? icons[url] : "";
            faviconCache.set(url, value);
            const resolvers = waiting.get(url) || [];
            waiting.delete(url);
            for (const resolve of resolvers)
                resolve(value);
        }
    }
}

function lookupFavicon(url) {
    if (faviconCache.has(url))
        return Promise.resolve(faviconCache.get(url));
    return new Promise(resolve => {
        if (!waiting.has(url))
            waiting.set(url, []);
        waiting.get(url).push(resolve);
        if (!flushScheduled) {
            flushScheduled = true;
            setTimeout(flushLookups, 0);
        }
    });
}

function isWebUrl(url) {
    return typeof url === "string" && /^https?:\/\//i.test(url);
}

function hostOf(url) {
    try {
        return new URL(url).hostname.replace(/^www\./, "");
    } catch {
        return "";
    }
}

export function initialOf(label, url) {
    const text = String(label || hostOf(url) || "?").trim();
    return (Array.from(text)[0] || "?").toUpperCase();
}

/**
 * A tile picturing a site. It shows `favicon` (base64 PNG) when given, or else looks one
 * up for `url`, and keeps `fallback` (a node; by default the initial of `label`) until a
 * logo arrives, or for good when there is none. Children appended to the tile later,
 * such as a pin mark, stay in place when the logo arrives.
 */
export function siteIcon({ className, url, label, favicon, fallback } = {}) {
    const el = document.createElement("span");
    el.className = className ? `${className} site-icon` : "site-icon";
    el.setAttribute("aria-hidden", "true");
    const fallbackNode = fallback || document.createTextNode(initialOf(label, url));
    el.append(fallbackNode);

    const show = base64 => {
        if (!base64 || fallbackNode.parentNode !== el)
            return;
        const img = document.createElement("img");
        img.alt = "";
        img.decoding = "async";
        img.addEventListener("error", () => {
            if (img.parentNode === el) {
                img.replaceWith(fallbackNode);
                el.classList.remove("has-logo");
            }
        }, { once: true });
        img.src = `data:image/png;base64,${base64}`;
        fallbackNode.replaceWith(img);
        el.classList.add("has-logo");
    };

    if (favicon)
        show(favicon);
    else if (isWebUrl(url))
        lookupFavicon(url).then(show);
    return el;
}
