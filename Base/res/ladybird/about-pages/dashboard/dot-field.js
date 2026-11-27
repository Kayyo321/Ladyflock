/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The pegboard: a field of dots at every grid intersection that tiles snap to.
 *
 * The field is drawn on one canvas behind the tiles. It only draws in response to
 * something the person did (entering edit mode, dragging, dropping, scrolling), and it
 * stops its animation loop as soon as every dot has settled.
 *
 * The canvas is a child of the board and scrolls with it. On a board taller than two
 * windows it covers only the stretch around what is in view and is moved along as the
 * page scrolls, so its size stays bounded however long the board grows.
 */

const BASE_RADIUS = 1.7;
const SETTLE_MS = 70;
const WAVE_SPEED = 2.2; // CSS px per ms
const MAX_WINDOW_HEIGHTS = 2;

function parseTriplet(value, fallback) {
    const parts = String(value || "").split(",").map(part => Number.parseFloat(part));
    if (parts.length >= 3 && parts.every(Number.isFinite))
        return parts.slice(0, 3);
    return fallback;
}

export function createDotField(canvas) {
    const g = canvas.getContext("2d");
    const board = canvas.parentElement;
    const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)");

    let geometry = null; // { cols, rows, unitX, unitY, gap, width, height, rtl }
    let pad = 12;
    let dots = []; // row-major: index = j * (cols + 1) + i, positions relative to the board
    let visible = false;
    let footprint = null; // { col, row, w, h, tone }
    let frame = 0;
    let lastTime = 0;
    let colors = { base: [128, 128, 128], accent: [10, 122, 255], warn: [200, 60, 50], alpha: 0.25 };

    // The stretch of the board the canvas currently covers, relative to the board's top.
    let sliceTop = 0;
    let sliceHeight = 0;
    let sliceWidth = 0;

    function readColors() {
        const style = getComputedStyle(canvas);
        colors = {
            base: parseTriplet(style.getPropertyValue("--dot-rgb"), [128, 128, 128]),
            accent: parseTriplet(style.getPropertyValue("--dot-accent-rgb"), [10, 122, 255]),
            warn: parseTriplet(style.getPropertyValue("--dot-warn-rgb"), [200, 60, 50]),
            alpha: Number.parseFloat(style.getPropertyValue("--dot-alpha")) || 0.25,
        };
    }

    // Sizes and places the canvas over the part of the board that is in view.
    // Returns true when the covered stretch changed.
    function placeCanvas(force = false) {
        if (!geometry)
            return false;
        const fullHeight = geometry.height + pad * 2;
        const windowHeight = window.innerHeight || 800;
        const width = geometry.width + pad * 2;

        let top = -pad;
        let height = fullHeight;
        if (fullHeight > windowHeight * MAX_WINDOW_HEIGHTS) {
            height = Math.ceil(windowHeight * MAX_WINDOW_HEIGHTS);
            const viewTop = -board.getBoundingClientRect().top;
            const covered = !force
                && sliceHeight === height
                && viewTop - windowHeight * 0.2 >= sliceTop
                && viewTop + windowHeight * 1.2 <= sliceTop + sliceHeight;
            if (covered && sliceWidth === width)
                return false;
            top = Math.round(viewTop - (height - windowHeight) / 2);
            top = Math.max(-pad, Math.min(top, geometry.height + pad - height));
        }

        if (!force && top === sliceTop && height === sliceHeight && width === sliceWidth)
            return false;

        sliceTop = top;
        sliceHeight = height;
        sliceWidth = width;
        const dpr = Math.min(window.devicePixelRatio || 1, 2);
        canvas.style.width = `${width}px`;
        canvas.style.height = `${height}px`;
        canvas.style.left = `${-pad}px`;
        canvas.style.top = `${top}px`;
        canvas.width = Math.round(width * dpr);
        canvas.height = Math.round(height * dpr);
        g.setTransform(dpr, 0, 0, dpr, 0, 0);
        return true;
    }

    function setGeometry(next) {
        const changed = !geometry
            || geometry.cols !== next.cols
            || geometry.rows !== next.rows
            || Math.abs(geometry.unitX - next.unitX) > 0.01
            || Math.abs(geometry.unitY - next.unitY) > 0.01
            || geometry.gap !== next.gap
            || geometry.rtl !== next.rtl
            || Math.abs(geometry.width - next.width) > 0.01;
        const previous = geometry;
        geometry = { ...next };
        if (!changed)
            return;

        pad = Math.ceil(next.gap / 2) + 9;

        // Rows are added and removed at the bottom as tiles move, so keep the state of the dots
        // that are still there. Any other change starts the field afresh.
        const keep = previous
            && previous.cols === next.cols
            && previous.rtl === next.rtl
            && Math.abs(previous.unitX - next.unitX) <= 0.01
            && Math.abs(previous.unitY - next.unitY) <= 0.01
            && previous.gap === next.gap;
        const old = keep ? dots : [];
        dots = [];
        for (let j = 0; j <= next.rows; ++j) {
            for (let i = 0; i <= next.cols; ++i) {
                const index = j * (next.cols + 1) + i;
                const prior = old[index];
                dots.push({
                    i,
                    j,
                    // Column 0 is at the inline start, which is the right edge in a right-to-left page.
                    x: next.rtl
                        ? next.width - i * next.unitX + next.gap / 2
                        : i * next.unitX - next.gap / 2,
                    y: j * next.unitY - next.gap / 2,
                    r: prior ? prior.r : BASE_RADIUS,
                    a: prior ? prior.a : (visible ? 1 : 0),
                    k: prior ? prior.k : 0,
                    tr: BASE_RADIUS,
                    ta: visible ? 1 : 0,
                    tk: 0,
                    tone: 0,
                    revealAt: prior ? prior.revealAt : 0,
                });
            }
        }
        placeCanvas(true);
        computeTargets();
        requestDraw();
    }

    function computeTargets() {
        for (const dot of dots) {
            dot.tr = BASE_RADIUS;
            dot.ta = visible ? 1 : 0;
            dot.tk = 0;
            if (!visible || !footprint)
                continue;

            const left = footprint.col;
            const right = footprint.col + footprint.w;
            const top = footprint.row;
            const bottom = footprint.row + footprint.h;
            const dx = dot.i < left ? left - dot.i : dot.i > right ? dot.i - right : 0;
            const dy = dot.j < top ? top - dot.j : dot.j > bottom ? dot.j - bottom : 0;
            dot.tone = footprint.tone === "warn" ? 1 : 0;

            if (dx === 0 && dy === 0) {
                const onColumnEdge = dot.i === left || dot.i === right;
                const onRowEdge = dot.j === top || dot.j === bottom;
                if (onColumnEdge && onRowEdge) {
                    dot.tr = 4.4;
                    dot.tk = 1;
                } else if (onColumnEdge || onRowEdge) {
                    dot.tr = 2.7;
                    dot.tk = 1;
                } else {
                    dot.tr = 1.8;
                    dot.tk = 0.7;
                }
                continue;
            }

            const distance = Math.hypot(dx, dy);
            if (distance <= 2.3) {
                const nearness = 1 - distance / 2.3;
                dot.tr = BASE_RADIUS + nearness * 0.9;
                dot.tk = nearness * 0.55;
            }
        }
    }

    // Dots outside the covered stretch are not animated; they take up their resting state at once.
    function settleAll() {
        for (const dot of dots) {
            dot.revealAt = 0;
            dot.r = dot.tr;
            dot.a = dot.ta;
            dot.k = dot.tk;
        }
    }

    function draw(now) {
        frame = 0;
        if (!geometry)
            return;
        const dt = lastTime ? Math.min(64, now - lastTime) : 16;
        lastTime = now;
        const instant = reducedMotion.matches;
        const ease = instant ? 1 : 1 - Math.exp(-dt / SETTLE_MS);

        let moving = false;
        g.clearRect(0, 0, sliceWidth, sliceHeight);

        // Only the rows under the canvas are worked on.
        const perRow = geometry.cols + 1;
        const firstRow = Math.max(0, Math.floor((sliceTop - 8 + geometry.gap / 2) / geometry.unitY));
        const lastRow = Math.min(geometry.rows, Math.ceil((sliceTop + sliceHeight + 8 + geometry.gap / 2) / geometry.unitY));

        const [br, bg, bb] = colors.base;
        for (let index = firstRow * perRow, end = Math.min(dots.length, (lastRow + 1) * perRow); index < end; ++index) {
            const dot = dots[index];
            if (dot.revealAt > now && !instant) {
                moving = true;
                continue;
            }
            if (dot.revealAt) {
                // The wavefront arrives: the dot blooms, then settles.
                dot.revealAt = 0;
                dot.r = BASE_RADIUS * 2.6;
                dot.a = 1;
            }

            dot.r += (dot.tr - dot.r) * ease;
            dot.a += (dot.ta - dot.a) * ease;
            dot.k += (dot.tk - dot.k) * ease;
            if (Math.abs(dot.tr - dot.r) > 0.02 || Math.abs(dot.ta - dot.a) > 0.01 || Math.abs(dot.tk - dot.k) > 0.01) {
                moving = true;
            } else {
                dot.r = dot.tr;
                dot.a = dot.ta;
                dot.k = dot.tk;
            }

            if (dot.a <= 0.005)
                continue;

            const [ar, ag, ab] = dot.tone ? colors.warn : colors.accent;
            const k = dot.k;
            const red = Math.round(br + (ar - br) * k);
            const green = Math.round(bg + (ag - bg) * k);
            const blue = Math.round(bb + (ab - bb) * k);
            const alpha = (colors.alpha + (1 - colors.alpha) * k) * dot.a;
            g.fillStyle = `rgba(${red}, ${green}, ${blue}, ${alpha.toFixed(3)})`;
            g.beginPath();
            g.arc(dot.x + pad, dot.y - sliceTop, Math.max(0.2, dot.r), 0, Math.PI * 2);
            g.fill();
        }

        if (moving)
            requestDraw();
        else
            lastTime = 0;
    }

    function requestDraw() {
        if (!frame)
            frame = requestAnimationFrame(draw);
    }

    function show(origin = null) {
        if (visible)
            return;
        visible = true;
        readColors();
        placeCanvas();
        const now = performance.now();
        if (origin && geometry && !reducedMotion.matches) {
            const rect = board.getBoundingClientRect();
            const ox = origin.clientX - rect.left;
            const oy = origin.clientY - rect.top;
            for (const dot of dots) {
                dot.revealAt = now + Math.hypot(dot.x - ox, dot.y - oy) / WAVE_SPEED;
                dot.a = 0;
            }
        }
        computeTargets();
        requestDraw();
    }

    function hide() {
        if (!visible)
            return;
        visible = false;
        footprint = null;
        for (const dot of dots)
            dot.revealAt = 0;
        computeTargets();
        requestDraw();
    }

    function setFootprint(next) {
        const same = (!next && !footprint)
            || (next && footprint
                && next.col === footprint.col && next.row === footprint.row
                && next.w === footprint.w && next.h === footprint.h
                && (next.tone || "") === (footprint.tone || ""));
        if (same)
            return;
        footprint = next ? { ...next } : null;
        computeTargets();
        requestDraw();
    }

    // A tile has just landed on `rect`: its pegs swell once and settle.
    function pulse(rect) {
        if (!visible || reducedMotion.matches)
            return;
        for (const dot of dots) {
            const onColumn = dot.i === rect.col || dot.i === rect.col + rect.w;
            const onRow = dot.j === rect.row || dot.j === rect.row + rect.h;
            const inColumns = dot.i >= rect.col && dot.i <= rect.col + rect.w;
            const inRows = dot.j >= rect.row && dot.j <= rect.row + rect.h;
            if (onColumn && onRow) {
                dot.r = 6.5;
                dot.k = 1;
                dot.tone = 0;
            } else if ((onColumn && inRows) || (onRow && inColumns)) {
                dot.r = 3.4;
                dot.k = 0.9;
                dot.tone = 0;
            }
        }
        requestDraw();
    }

    function refreshColors() {
        readColors();
        requestDraw();
    }

    function onViewportChange() {
        if (!geometry || (!visible && !frame))
            return;
        if (placeCanvas()) {
            settleAll();
            requestDraw();
        }
    }

    readColors();
    window.matchMedia("(prefers-color-scheme: dark)").addEventListener("change", refreshColors);
    window.addEventListener("scroll", onViewportChange, { passive: true });
    window.addEventListener("resize", onViewportChange);

    return { setGeometry, show, hide, setFootprint, pulse, refreshColors, isVisible: () => visible };
}
