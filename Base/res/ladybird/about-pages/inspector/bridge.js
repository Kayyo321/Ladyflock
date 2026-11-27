/*
 * Copyright (c) 2026, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Typed WebUI bridge, safe DOM helpers, and WCAG 2 / APCA contrast math.
 */

let nextRequestId = 1;
let documentGeneration;
const pendingRequests = new Map();
const eventListeners = new Map();
const timedReads = new Set(["getDomTree", "getNodeHtml", "getAccessibilityTree", "listStyleSheets", "getStyleSheetSource", "getSourceContent"]);

export function onEvent(name, handler) {
    if (!eventListeners.has(name))
        eventListeners.set(name, new Set());
    eventListeners.get(name).add(handler);
    return () => eventListeners.get(name)?.delete(handler);
}

function emitEvent(name, data) {
    const set = eventListeners.get(name);
    if (!set)
        return;
    for (const fn of set) {
        try {
            fn(data);
        } catch (err) {
            console.error(`Inspector listener error for ${name}:`, err);
        }
    }
}

export function sendCommand(command, params = {}) {
    return new Promise((resolve, reject) => {
        if (!window.ladybird || typeof window.ladybird.sendMessage !== "function") {
            reject(new Error("WebUI bridge is unavailable."));
            return;
        }
        const requestId = nextRequestId++;
        const timeout = timedReads.has(command) ? setTimeout(() => {
            pendingRequests.delete(requestId);
            reject(new Error("The target did not return the requested data in time."));
        }, 10000) : undefined;
        pendingRequests.set(requestId, { resolve, reject, timeout });
        try {
            window.ladybird.sendMessage("inspectorCommand", {
                requestId,
                command,
                params: documentGeneration === undefined ? params : { documentGeneration, ...params },
            });
        } catch (err) {
            pendingRequests.delete(requestId);
            clearTimeout(timeout);
            reject(err);
        }
    });
}

export function sendEngineCommand(action, params = {}) {
    return sendCommand("engineCommand", { ...params, action });
}

export function initBridge() {
    document.addEventListener("WebUIMessage", event => {
        const { name, data } = event.detail || {};
        if (name === "inspectorState" || name === "targetDocumentChanged" || name === "targetCrashed" || name === "targetRecovered")
            documentGeneration = data?.documentGeneration;
        if (["targetDocumentChanged", "targetCrashed", "targetRecovered", "targetClosed"].includes(name)
            || (name === "inspectorState" && !data?.bound)) {
            for (const { reject, timeout } of pendingRequests.values()) {
                clearTimeout(timeout);
                reject(new Error("The inspected document changed before the command returned."));
            }
            pendingRequests.clear();
        }
        if (name === "inspectorReply" && data) {
            const pending = pendingRequests.get(data.requestId);
            if (pending) {
                pendingRequests.delete(data.requestId);
                clearTimeout(pending.timeout);
                if (data.ok)
                    pending.resolve(data.result);
                else
                    pending.reject(new Error(data.error || "Inspector command failed"));
            }
            return;
        }
        emitEvent(name, data);
    });
}

// Inert element builder — never uses innerHTML on inspected page data.
export function el(tag, attrs = {}, ...children) {
    const node = document.createElement(tag);
    for (const [k, v] of Object.entries(attrs)) {
        if (v === undefined || v === null || v === false)
            continue;
        if (k === "class")
            node.className = v;
        else if (k === "style")
            throw new Error("el(): the Inspector's CSP blocks style attributes; use a class, or node.style for computed values.");
        else if (k === "text")
            node.textContent = String(v);
        else if (k.startsWith("on") && typeof v === "function")
            node.addEventListener(k.slice(2).toLowerCase(), v);
        else if (k === "dataset" && typeof v === "object") {
            for (const [dk, dv] of Object.entries(v))
                node.dataset[dk] = String(dv);
        } else if (v === true)
            node.setAttribute(k, "");
        else
            node.setAttribute(k, String(v));
    }
    for (const child of children) {
        if (child === undefined || child === null || child === false)
            continue;
        if (typeof child === "string" || typeof child === "number")
            node.appendChild(document.createTextNode(String(child)));
        else
            node.appendChild(child);
    }
    return node;
}

// A strip that only scrolls sideways (tabs, breadcrumbs): a vertical wheel or two-finger swipe moves it sideways
// instead of doing nothing, and a fade at either edge shows that more is hidden past it.
export function enhanceScrollStrip(strip) {
    const updateFades = () => {
        const overflowing = strip.scrollWidth > strip.clientWidth + 1;
        strip.classList.toggle("tabs-fade-start", overflowing && strip.scrollLeft > 1);
        strip.classList.toggle("tabs-fade-end", overflowing && strip.scrollLeft + strip.clientWidth < strip.scrollWidth - 1);
    };
    strip.addEventListener("wheel", e => {
        if (strip.scrollWidth <= strip.clientWidth || Math.abs(e.deltaY) <= Math.abs(e.deltaX))
            return;
        e.preventDefault();
        strip.scrollLeft += e.deltaMode === WheelEvent.DOM_DELTA_LINE ? e.deltaY * 16 : e.deltaY;
    }, { passive: false });
    strip.addEventListener("scroll", updateFades, { passive: true });
    new ResizeObserver(updateFades).observe(strip);
    new MutationObserver(updateFades).observe(strip, { childList: true });
    updateFades();
    return updateFades;
}

// Scrolls a strip just enough to show the given child, without moving any other scroller.
export function revealInStrip(strip, child) {
    if (!strip || !child)
        return;
    const visible = strip.getBoundingClientRect();
    const target = child.getBoundingClientRect();
    if (target.left < visible.left)
        strip.scrollLeft -= visible.left - target.left + 16;
    else if (target.right > visible.right)
        strip.scrollLeft += target.right - visible.right + 16;
}

// ARIA tabs: one Tab stop for the list; arrows, Home and End move between tabs and activate them.
export function enableTabListKeyboard(tablist, activate) {
    const tabs = () => [...tablist.querySelectorAll('[role="tab"]')];
    const syncTabStops = () => {
        for (const tab of tabs())
            tab.tabIndex = tab.getAttribute("aria-selected") === "true" ? 0 : -1;
    };
    new MutationObserver(syncTabStops).observe(tablist, { subtree: true, attributeFilter: ["aria-selected"] });
    syncTabStops();
    tablist.addEventListener("keydown", e => {
        const list = tabs();
        const index = list.indexOf(document.activeElement);
        if (index < 0)
            return;
        let next = null;
        if (e.key === "ArrowRight")
            next = list[(index + 1) % list.length];
        else if (e.key === "ArrowLeft")
            next = list[(index - 1 + list.length) % list.length];
        else if (e.key === "Home")
            next = list[0];
        else if (e.key === "End")
            next = list[list.length - 1];
        if (!next)
            return;
        e.preventDefault();
        activate(next);
        next.focus();
    });
}

export function announce(message) {
    const region = document.getElementById("live-announcer");
    if (!region)
        return;
    region.textContent = "";
    requestAnimationFrame(() => {
        region.textContent = message;
    });
}

// Parse hex / rgb() / rgba() into { r, g, b, a } in [0..255] / [0..1].
export function parseCssColor(raw) {
    if (!raw || typeof raw !== "string")
        return null;
    const s = raw.trim().toLowerCase();
    if (s === "transparent")
        return { r: 0, g: 0, b: 0, a: 0 };

    const hex = s.match(/^#([0-9a-f]{3}|[0-9a-f]{4}|[0-9a-f]{6}|[0-9a-f]{8})$/);
    if (hex) {
        const h = hex[1];
        if (h.length === 3 || h.length === 4) {
            return {
                r: parseInt(h[0] + h[0], 16),
                g: parseInt(h[1] + h[1], 16),
                b: parseInt(h[2] + h[2], 16),
                a: h.length === 4 ? parseInt(h[3] + h[3], 16) / 255 : 1,
            };
        }
        return {
            r: parseInt(h.slice(0, 2), 16),
            g: parseInt(h.slice(2, 4), 16),
            b: parseInt(h.slice(4, 6), 16),
            a: h.length === 8 ? parseInt(h.slice(6, 8), 16) / 255 : 1,
        };
    }

    const rgb = s.match(/^rgba?\(\s*([\d.]+)[,\s]+([\d.]+)[,\s]+([\d.]+)(?:\s*[,/]\s*([\d.]+%?))?\s*\)$/);
    if (rgb) {
        let a = 1;
        if (rgb[4] !== undefined)
            a = rgb[4].endsWith("%") ? parseFloat(rgb[4]) / 100 : parseFloat(rgb[4]);
        return {
            r: Math.min(255, Math.max(0, Number(rgb[1]))),
            g: Math.min(255, Math.max(0, Number(rgb[2]))),
            b: Math.min(255, Math.max(0, Number(rgb[3]))),
            a: Math.min(1, Math.max(0, a)),
        };
    }
    return null;
}

export function compositeOver(fg, bg) {
    const a = fg.a + bg.a * (1 - fg.a);
    if (a <= 0)
        return { r: 0, g: 0, b: 0, a: 0 };
    return {
        r: (fg.r * fg.a + bg.r * bg.a * (1 - fg.a)) / a,
        g: (fg.g * fg.a + bg.g * bg.a * (1 - fg.a)) / a,
        b: (fg.b * fg.a + bg.b * bg.a * (1 - fg.a)) / a,
        a,
    };
}

function relativeLuminance({ r, g, b }) {
    const lin = c => {
        const v = c / 255;
        return v <= 0.04045 ? v / 12.92 : ((v + 0.055) / 1.055) ** 2.4;
    };
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

// Returns WCAG 2 contrast ratio and APCA Lc estimate, or null when background is non-solid/unknown.
export function computeContrast(fgRaw, bgRaw, { hasBackgroundImage = false, hasBlendMode = false, fontSizePx = 14, fontWeight = 400 } = {}) {
    if (hasBackgroundImage || hasBlendMode)
        return { unavailable: true, reason: "Contrast unavailable (image, gradient, or blend mode)" };

    const fgParsed = parseCssColor(fgRaw);
    const bgParsed = parseCssColor(bgRaw);
    if (!fgParsed || !bgParsed)
        return { unavailable: true, reason: "Contrast unavailable" };

    if (bgParsed.a < 1)
        return { unavailable: true, reason: "Contrast unavailable (underlying background unknown)" };
    const opaqueBg = bgParsed;
    const effectiveFg = fgParsed.a < 1 ? compositeOver(fgParsed, opaqueBg) : fgParsed;

    const l1 = relativeLuminance(effectiveFg);
    const l2 = relativeLuminance(opaqueBg);
    const ratio = (Math.max(l1, l2) + 0.05) / (Math.min(l1, l2) + 0.05);

    const isLargeText = fontSizePx >= 24 || (fontSizePx >= 18.66 && fontWeight >= 700);
    const aaThreshold = isLargeText ? 3.0 : 4.5;
    const aaaThreshold = isLargeText ? 4.5 : 7.0;

    // APCA-W3 (0.0.98G-4g) lightness contrast estimate
    const sY = ({ r, g, b }) => 0.2126729 * (r / 255) ** 2.4 + 0.7151522 * (g / 255) ** 2.4 + 0.072175 * (b / 255) ** 2.4;
    let yTxt = sY(effectiveFg);
    let yBg = sY(opaqueBg);
    if (yTxt < 0.022)
        yTxt += (0.022 - yTxt) ** 1.414;
    if (yBg < 0.022)
        yBg += (0.022 - yBg) ** 1.414;
    let sapc = 0;
    if (Math.abs(yBg - yTxt) >= 0.0005) {
        if (yBg > yTxt)
            sapc = (yBg ** 0.56 - yTxt ** 0.57) * 1.14;
        else
            sapc = (yBg ** 0.65 - yTxt ** 0.62) * 1.14;
    }
    let lc = 0;
    if (Math.abs(sapc) >= 0.1)
        lc = (sapc > 0 ? sapc - 0.027 : sapc + 0.027) * 100;

    return {
        unavailable: false,
        ratio,
        aa: ratio >= aaThreshold,
        aaa: ratio >= aaaThreshold,
        aaThreshold,
        aaaThreshold,
        isLargeText,
        apcaLc: Math.round(lc),
    };
}

export function rgbToHex({ r, g, b }) {
    const h = n => Math.round(n).toString(16).padStart(2, "0");
    return `#${h(r)}${h(g)}${h(b)}`;
}
