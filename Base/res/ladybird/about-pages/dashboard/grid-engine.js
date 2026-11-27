/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Pure deterministic, idempotent grid placement solver for about:dashboard.
 */

export const MAX_GRID_ROWS = 512;

export const BREAKPOINTS = [
    { name: "wide", minWidth: 1120, cols: 12, rowHeight: 96, gap: 14 },
    { name: "medium", minWidth: 800, cols: 8, rowHeight: 96, gap: 12 },
    { name: "narrow", minWidth: 520, cols: 4, rowHeight: 92, gap: 12 },
    { name: "compact", minWidth: 0, cols: 2, rowHeight: 88, gap: 10 },
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
        const w = Math.max(1, Math.min(sz.w, cols));
        const h = Math.max(1, sz.h);
        const key = `${w}x${h}`;
        if (seen.has(key))
            continue;
        seen.add(key);
        out.push({ w, h, label: sz.label || `${w}×${h}`, tier: sz.tier || "default" });
    }
    return out;
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

function collidesWithAny(candidate, placed) {
    for (const p of placed) {
        if (rectsOverlap(candidate, p))
            return true;
    }
    return false;
}

export function findFirstFreeSlot(existingPlacements, w, h, cols) {
    const cw = Math.max(1, Math.min(w, cols));
    const ch = Math.max(1, h);
    for (let row = 0; row + ch <= MAX_GRID_ROWS; ++row) {
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
 * - Pins `pinnedIds` (clamped inside bounds), verifying they do not overlap each other.
 * - Places remaining cards in their prior stable reading order at their prior column,
 *   shifting downward to the first non-overlapping row >= prior row.
 * - If `compactMode` is true, pulls non-pinned cards upward into legal gaps in reading order.
 * Returns `{ ok: true, placements }` or `{ ok: false, error }`.
 */
export function resolveLayout(inputPlacements, pinnedIds, cols, compactMode = false) {
    const pinnedSet = new Set(pinnedIds || []);
    const ordered = sortReadingOrder(inputPlacements);
    const placed = [];

    // 1. Place pinned targets first and verify mutual non-overlap and bounds.
    for (const item of ordered) {
        if (!pinnedSet.has(item.instanceId))
            continue;
        const w = Math.max(1, Math.min(item.w, cols));
        const h = Math.max(1, item.h);
        const col = Math.max(0, Math.min(item.col, cols - w));
        const row = Math.max(0, item.row);
        if (row + h > MAX_GRID_ROWS)
            return { ok: false, error: "Layout exceeds maximum grid height." };
        const rect = { instanceId: item.instanceId, col, row, w, h };
        if (collidesWithAny(rect, placed))
            return { ok: false, error: "Pinned cards overlap." };
        placed.push(rect);
    }

    // 2. Place remaining cards in stable reading order, shifting down if obstructed.
    for (const item of ordered) {
        if (pinnedSet.has(item.instanceId))
            continue;
        const w = Math.max(1, Math.min(item.w, cols));
        const h = Math.max(1, item.h);
        const col = Math.max(0, Math.min(item.col, cols - w));
        let row = Math.max(0, item.row);
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
            for (let r = 0; r < p.row; ++r) {
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
 * `(row, col, instanceId)` reading order using first free row-major slot.
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
    return sortReadingOrder(placed);
}

/**
 * Swaps positions of two placed cards:
 * - Same-size cards exchange coordinates exactly.
 * - Unequal-size cards retain their sizes, pin both anchors (clamped if needed),
 *   reject if the two pinned rects overlap or cross bounds, and resolve remaining cards.
 */
export function swapPlacements(currentPlacements, idA, idB, cols, compactMode = false) {
    const a = currentPlacements.find(p => p.instanceId === idA);
    const b = currentPlacements.find(p => p.instanceId === idB);
    if (!a || !b || idA === idB)
        return { ok: false, error: "Select two distinct cards to swap." };

    if (a.w === b.w && a.h === b.h) {
        const next = currentPlacements.map(p => {
            if (p.instanceId === idA)
                return { ...p, col: b.col, row: b.row };
            if (p.instanceId === idB)
                return { ...p, col: a.col, row: a.row };
            return { ...p };
        });
        return { ok: true, placements: sortReadingOrder(next) };
    }

    const newA = { ...a, col: b.col, row: b.row };
    const newB = { ...b, col: a.col, row: a.row };
    if (newA.col + newA.w > cols || newB.col + newB.w > cols || newA.row + newA.h > MAX_GRID_ROWS || newB.row + newB.h > MAX_GRID_ROWS) {
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
    return resolveLayout(next, [idA, idB], cols, compactMode);
}
