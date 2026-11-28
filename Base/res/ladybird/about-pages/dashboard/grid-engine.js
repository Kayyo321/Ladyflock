/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Pure deterministic, idempotent grid placement solver for about:dashboard.
 */

// A stored coordinate is one quarter of a catalog cell. Resize steps are two
// coordinates wide so a resized tile's center is still addressable when moving it.
export const GRID_STEPS_PER_CELL = 4;
export const GRID_RESIZE_STEP = 2;
export const MAX_GRID_ROWS = 512 * GRID_STEPS_PER_CELL;
export const MIN_GRID_ROW = -MAX_GRID_ROWS;

export const BREAKPOINTS = [
    { name: "wide", minWidth: 1120, cols: 48, rowHeight: 96, gap: 14 },
    { name: "medium", minWidth: 800, cols: 32, rowHeight: 96, gap: 12 },
    { name: "narrow", minWidth: 520, cols: 16, rowHeight: 92, gap: 12 },
    { name: "compact", minWidth: 0, cols: 8, rowHeight: 88, gap: 10 },
];

export function getBreakpointForWidth(widthPx) {
    for (const bp of BREAKPOINTS) {
        if (widthPx >= bp.minWidth)
            return bp;
    }
    return BREAKPOINTS[BREAKPOINTS.length - 1];
}

export function getBreakpointByName(name) {
    return BREAKPOINTS.find(bp => bp.name === name) || BREAKPOINTS[0];
}

export function effectiveSizesForBreakpoint(sizes, cols) {
    const out = [];
    const seen = new Set();
    for (const sz of sizes) {
        const w = Math.max(1, Math.min(sz.w * GRID_STEPS_PER_CELL, cols));
        const h = Math.max(1, sz.h * GRID_STEPS_PER_CELL);
        const key = `${w}x${h}`;
        if (seen.has(key))
            continue;
        seen.add(key);
        out.push({ w, h, label: formatGridSize({ w, h }), tier: sz.tier || "default" });
    }
    return out;
}

export function formatGridSize({ w, h }, separator = " × ") {
    return [w / GRID_STEPS_PER_CELL, h / GRID_STEPS_PER_CELL].join(separator);
}

export function resizeLimits(placement, sizes, cols) {
    const maxW = Math.min(cols - placement.col, Math.max(placement.w, ...sizes.map(s => s.w)));
    const maxH = Math.min(MAX_GRID_ROWS, MAX_GRID_ROWS - placement.row, Math.max(placement.h, ...sizes.map(s => s.h)));
    return {
        minW: Math.min(placement.w, ...sizes.map(s => s.w)),
        minH: Math.min(placement.h, ...sizes.map(s => s.h)),
        maxW: Math.floor(maxW / GRID_RESIZE_STEP) * GRID_RESIZE_STEP,
        maxH: Math.floor(maxH / GRID_RESIZE_STEP) * GRID_RESIZE_STEP,
    };
}

export function snapResizePlacement(start, w, h, { minW, minH, maxW, maxH }) {
    return {
        col: start.col,
        row: start.row,
        w: Math.max(minW, Math.min(maxW, Math.round(w / GRID_RESIZE_STEP) * GRID_RESIZE_STEP)),
        h: Math.max(minH, Math.min(maxH, Math.round(h / GRID_RESIZE_STEP) * GRID_RESIZE_STEP)),
    };
}

export function snapResizeSize(targetW, targetH, validSizes) {
    if (!validSizes.length)
        return { w: Math.max(1, targetW), h: Math.max(1, targetH) };
    let best = validSizes[0];
    let bestDw = Math.abs(best.w - targetW);
    let bestDh = Math.abs(best.h - targetH);
    for (let i = 1; i < validSizes.length; ++i) {
        const cand = validSizes[i];
        const dw = Math.abs(cand.w - targetW);
        const dh = Math.abs(cand.h - targetH);
        if (dw < bestDw || (dw === bestDw && dh < bestDh)) {
            best = cand;
            bestDw = dw;
            bestDh = dh;
        }
    }
    return { w: best.w, h: best.h };
}

export function rectsOverlap(a, b) {
    return a.col < b.col + b.w
        && a.col + a.w > b.col
        && a.row < b.row + b.h
        && a.row + a.h > b.row;
}

export function sortReadingOrder(placements) {
    return [...placements].sort((a, b) => {
        if (a.row !== b.row)
            return a.row - b.row;
        if (a.col !== b.col)
            return a.col - b.col;
        return a.instanceId.localeCompare(b.instanceId);
    });
}

export function centerPlacements(placements) {
    if (!placements.length)
        return [];

    let top = Infinity;
    let bottom = -Infinity;
    for (const { row, h } of placements) {
        top = Math.min(top, row);
        bottom = Math.max(bottom, row + h);
    }
    const centerRow = Math.round((top + bottom) / 2);
    return placements.map(placement => ({ ...placement, row: placement.row - centerRow }));
}

function collidesWithAny(candidate, placed) {
    for (const p of placed) {
        if (rectsOverlap(candidate, p))
            return true;
    }
    return false;
}

export function findFirstFreeSlot(existingPlacements, w, h, cols, minRow = 0) {
    const cw = Math.max(1, Math.min(w, cols));
    const ch = Math.max(1, h);
    if (ch > MAX_GRID_ROWS)
        return null;
    for (let row = Math.max(MIN_GRID_ROW, minRow); row + ch <= MAX_GRID_ROWS; ++row) {
        for (let col = 0; col + cw <= cols; ++col) {
            const candidate = { col, row, w: cw, h: ch };
            if (!collidesWithAny(candidate, existingPlacements))
                return candidate;
        }
    }
    return null;
}

/**
 * Deterministic, idempotent placement solver:
 * - Pins `pinnedIds` (clamped inside bounds), verifying they do not overlap each other. These are
 *   the tile being placed and every locked tile: nothing the solver does moves them.
 * - Places remaining cards in their prior stable reading order at their prior column,
 *   shifting downward to the first non-overlapping row >= prior row.
 * - If `compactMode` is true, pulls non-pinned cards upward into legal gaps in reading order.
 * Returns `{ ok: true, placements }` or `{ ok: false, error }`.
 */
export function resolveLayout(inputPlacements, pinnedIds, cols, compactMode = false, minRow = MIN_GRID_ROW) {
    const pinnedSet = new Set(pinnedIds || []);
    const ordered = sortReadingOrder(inputPlacements);
    const placed = [];
    const firstRow = Math.max(MIN_GRID_ROW, minRow);

    // 1. Place pinned targets first and verify mutual non-overlap and bounds.
    for (const item of ordered) {
        if (!pinnedSet.has(item.instanceId))
            continue;
        const w = Math.max(1, Math.min(item.w, cols));
        const h = Math.max(1, item.h);
        const col = Math.max(0, Math.min(item.col, cols - w));
        const row = Math.max(firstRow, item.row);
        if (h > MAX_GRID_ROWS || row + h > MAX_GRID_ROWS)
            return { ok: false, error: "Layout exceeds maximum grid height." };
        const rect = { instanceId: item.instanceId, col, row, w, h };
        if (collidesWithAny(rect, placed))
            return { ok: false, error: "A locked tile is in the way." };
        placed.push(rect);
    }

    // 2. Place remaining cards in stable reading order, shifting down if obstructed.
    for (const item of ordered) {
        if (pinnedSet.has(item.instanceId))
            continue;
        const w = Math.max(1, Math.min(item.w, cols));
        const h = Math.max(1, item.h);
        const col = Math.max(0, Math.min(item.col, cols - w));
        let row = Math.max(firstRow, item.row);
        if (h > MAX_GRID_ROWS)
            return { ok: false, error: "Layout exceeds maximum grid height." };
        while (row + h <= MAX_GRID_ROWS && collidesWithAny({ col, row, w, h }, placed)) {
            ++row;
        }
        if (row + h > MAX_GRID_ROWS)
            return { ok: false, error: "Layout exceeds maximum grid height." };
        placed.push({ instanceId: item.instanceId, col, row, w, h });
    }

    // 3. Optional compaction pass (moves non-pinned cards upward into legal gaps).
    if (compactMode) {
        const sortedAfter = sortReadingOrder(placed);
        const compacted = [];
        // Keep pinned cards fixed in place during compaction.
        for (const p of sortedAfter) {
            if (pinnedSet.has(p.instanceId))
                compacted.push({ ...p });
        }
        for (const p of sortedAfter) {
            if (pinnedSet.has(p.instanceId))
                continue;
            let bestRow = p.row;
            for (let r = firstRow; r < p.row; ++r) {
                if (!collidesWithAny({ col: p.col, row: r, w: p.w, h: p.h }, compacted)) {
                    bestRow = r;
                    break;
                }
            }
            compacted.push({ ...p, row: bestRow });
        }
        return { ok: true, placements: sortReadingOrder(compacted) };
    }

    return { ok: true, placements: sortReadingOrder(placed) };
}

/**
 * Projects canonical wide placements onto a narrower breakpoint in stable
 * `(row, col, instanceId)` reading order using first free row-major slot, then
 * centers the projected group relative to row zero.
 */
export function projectPlacements(widePlacements, targetCols) {
    const ordered = sortReadingOrder(widePlacements);
    const placed = [];
    for (const item of ordered) {
        const w = Math.max(1, Math.min(item.w, targetCols));
        const h = Math.max(1, item.h);
        const slot = findFirstFreeSlot(placed, w, h, targetCols);
        if (!slot)
            break;
        placed.push({ instanceId: item.instanceId, ...slot });
    }
    return centerPlacements(sortReadingOrder(placed));
}

/**
 * Swaps positions of two placed cards:
 * - Same-size cards exchange coordinates exactly.
 * - Unequal-size cards retain their sizes, pin both anchors,
 *   reject if the two pinned rects overlap or cross bounds, and resolve remaining cards
 *   around them and around `fixedIds`, which stay where they are.
 */
export function swapPlacements(currentPlacements, idA, idB, cols, compactMode = false, minRow = MIN_GRID_ROW, fixedIds = []) {
    const a = currentPlacements.find(p => p.instanceId === idA);
    const b = currentPlacements.find(p => p.instanceId === idB);
    if (!a || !b || idA === idB)
        return { ok: false, error: "Select two distinct cards to swap." };

    const newA = { ...a, col: b.col, row: b.row };
    const newB = { ...b, col: a.col, row: a.row };
    const firstRow = Math.max(MIN_GRID_ROW, minRow);
    if ([newA, newB].some(p => p.col + p.w > cols || p.row < firstRow || p.h > MAX_GRID_ROWS || p.row + p.h > MAX_GRID_ROWS)) {
        return { ok: false, error: "These two cards don’t fit cleanly in each other’s anchor positions." };
    }
    if (rectsOverlap(newA, newB)) {
        return { ok: false, error: "Swapping these two cards would make them overlap." };
    }

    const next = currentPlacements.map(p => {
        if (p.instanceId === idA)
            return newA;
        if (p.instanceId === idB)
            return newB;
        return { ...p };
    });
    if (a.w === b.w && a.h === b.h)
        return { ok: true, placements: sortReadingOrder(next) };
    return resolveLayout(next, [idA, idB, ...fixedIds], cols, compactMode, firstRow);
}
