/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Main controller for about:dashboard.
 *
 * The dashboard is a pegboard. Tiles hang on a grid of pegs that stays out of sight
 * until the board is being edited. Smaller snap steps between the visible dots
 * allow precise placement and resizing without crowding the editing surface.
 */

import { onDashboardEvent, request, setExpectedGeneration, waitForBridge } from "./bridge.js";
import { createDotField } from "./dot-field.js";
import {
    GRID_RESIZE_STEP,
    GRID_STEPS_PER_CELL,
    MAX_GRID_ROWS,
    MIN_GRID_ROW,
    centerPlacements,
    effectiveSizesForBreakpoint,
    findFirstFreeSlot,
    formatGridSize,
    getBreakpointByName,
    getBreakpointForWidth,
    projectPlacements,
    resolveLayout,
    resizeLimits,
    snapResizePlacement,
    snapResizeSize,
    sortReadingOrder,
    swapPlacements,
} from "./grid-engine.js";
import { icon } from "./icons.js";
import {
    applyPlacementData,
    closeCardMenus,
    computeCardTier,
    createCardElement,
    isCardMenuOpen,
    openActionMenu,
    renderCardError,
} from "./widget-chrome.js";
import { createPickerController } from "./widget-picker.js";
import { SIZE_NAMES, getWidgetDef, getWidgetShortTitle, loadWidgetModule } from "./widget-registry.js";

const rootEl = document.documentElement;
const gridWrapEl = document.getElementById("dashboard-grid-wrap");
const boardEl = document.getElementById("dashboard-board");
const gridEl = document.getElementById("dashboard-grid");
const landingEl = document.getElementById("dashboard-landing");
const guideX = document.createElement("div");
const guideY = document.createElement("div");
guideX.className = "dash-guide dash-guide-vertical";
guideY.className = "dash-guide dash-guide-horizontal";
guideX.hidden = guideY.hidden = true;
document.getElementById("dashboard-guides").append(guideX, guideY);
const emptyEl = document.getElementById("dashboard-empty");
const bannerEl = document.getElementById("dashboard-banner");
const fallbackEl = document.getElementById("dashboard-fallback");
const footerEl = document.getElementById("dashboard-footer");
const saveStatusEl = document.getElementById("dashboard-save-status");
const swapBannerEl = document.getElementById("dashboard-swap-banner");
const swapTextEl = document.getElementById("dashboard-swap-text");
const swapCancelBtn = document.getElementById("dashboard-swap-cancel");
const editToolbarEl = document.getElementById("edit-toolbar");
const btnCustomize = document.getElementById("btn-customize");
const customizeWrapEl = document.getElementById("customize-wrap");
const btnCustomizeHide = document.getElementById("btn-customize-hide");
const btnDoneEdit = document.getElementById("btn-done-edit");
const btnAddWidget = document.getElementById("btn-add-widget");
const btnPresets = document.getElementById("btn-presets");
const btnCompact = document.getElementById("btn-compact");
const btnUndo = document.getElementById("btn-undo");
const btnRedo = document.getElementById("btn-redo");
const btnDeleteData = document.getElementById("btn-delete-data");
const emptyAddBtn = document.getElementById("empty-add-widget");
const emptyPresetBtn = document.getElementById("empty-choose-preset");
const announcerEl = document.getElementById("dashboard-announcer");

const shelfEl = document.getElementById("picker-dialog");

const configDialog = document.getElementById("config-dialog");
const configDialogTitle = document.getElementById("config-dialog-title");
const configDialogBody = document.getElementById("config-dialog-body");

const confirmDialog = document.getElementById("confirm-dialog");
const confirmTitle = document.getElementById("confirm-dialog-title");
const confirmMsg = document.getElementById("confirm-dialog-message");
const confirmExtra = document.getElementById("confirm-dialog-extra");
const confirmAccept = document.getElementById("confirm-dialog-accept");

const reducedMotion = window.matchMedia("(prefers-reduced-motion: reduce)");

// Glyphs for the controls declared in the page markup.
for (const el of document.querySelectorAll("[data-icon]"))
    el.prepend(icon(el.dataset.icon, 16));

for (const btn of document.querySelectorAll("[data-close-dialog]")) {
    btn.addEventListener("click", () => {
        const dlg = btn.closest("dialog");
        if (dlg && dlg.open)
            hideDialog(dlg);
    });
}

// A dialog's display is set from script as well as by its open state. The engine has been seen to
// keep a stale computed display on a dialog: painted over the board after its second close, and
// invisible when first opened after a preset was applied. An inline value is always honoured.
function showDialog(dialog) {
    dialog.style.display = "block";
    dialog.showModal();
}

// The close event comes a task later, and has been seen to leave the dialog laid out over the
// board, so a dialog the page closes itself loses its display at once.
function hideDialog(dialog) {
    if (dialog.open)
        dialog.close();
    dialog.style.display = "none";
}

for (const dialog of [configDialog, confirmDialog]) {
    dialog.style.display = "none";
    dialog.addEventListener("close", () => {
        dialog.style.display = "none";
    });
}

const dots = createDotField(document.getElementById("dashboard-dots"));

const picker = createPickerController({
    shelfEl,
    titleEl: document.getElementById("picker-title"),
    catsEl: document.getElementById("picker-cats"),
    tabWidgetsEl: document.getElementById("picker-tab-widgets"),
    tabPresetsEl: document.getElementById("picker-tab-presets"),
    searchInputEl: document.getElementById("picker-search-input"),
    bodyEl: document.getElementById("picker-body"),
    closeBtn: document.getElementById("picker-close"),
});

const INCOMING_ID = "~incoming";
const EXTRA_EDIT_ROWS = 2 * GRID_STEPS_PER_CELL;
const TILE_INLINE_INSET = 16;
const TILE_BOTTOM_CLEARANCE = 64;
const SETTLE_MS = 460;
const MAX_UNDO = 30;

let ctx = null;
let layout = { layoutRevision: 0, initialized: false, compactMode: false, widgets: [], placements: {} };
let currentBp = getBreakpointForWidth(window.innerWidth);
let isEditing = false;
// Whether the person hid the Customize button, for the session or for good.
let customizeHidden = false;
let selectedId = null;
let swapSourceId = null;
let undoStack = [];
let redoStack = [];
let opCounter = 1;
let hasRenderedOnce = false;
let domOrderTimer = 0;
let hoverCell = null;

// Mounted tiles keyed by instance ID.
const mountedCards = new Map();

// The drag, resize or drag-in gesture in progress.
let gestureState = null;

function announce(text) {
    announcerEl.textContent = "";
    requestAnimationFrame(() => {
        announcerEl.textContent = text;
    });
}

function setSaveStatus(statusText, temporary = false) {
    clearTimeout(setSaveStatus._timer);
    if (!statusText) {
        saveStatusEl.classList.add("hidden");
        saveStatusEl.textContent = "";
        return;
    }
    saveStatusEl.textContent = statusText;
    saveStatusEl.classList.remove("hidden");
    if (temporary)
        setSaveStatus._timer = setTimeout(updateDefaultSaveStatus, 2500);
}

function updateDefaultSaveStatus() {
    if (!ctx)
        return;
    if (ctx.isEphemeral)
        setSaveStatus("Session only");
    else if (ctx.storageStatus === "sql-disabled")
        setSaveStatus("Session only (SQL disabled)");
    else if (ctx.storageStatus === "corrupt-fallback")
        setSaveStatus("Session only (database preserved)");
    else if (ctx.storageStatus === "newer-schema")
        setSaveStatus("Session only (newer database version)");
    else if (ctx.storageStatus === "write-failed")
        setSaveStatus("Couldn’t save");
    else
        setSaveStatus("");
}

function updateBanner() {
    if (!ctx)
        return;
    let text = "";
    let glyph = "private";
    if (ctx.isPrivate) {
        text = "Private window · This board lasts for this private session only.";
    } else if (ctx.isEphemeral) {
        text = `Temporary session (${ctx.identityName}) · This board is cleared when the session ends.`;
    } else if (ctx.storageStatus && ctx.storageStatus !== "saved") {
        text = "Session only · Dashboard storage is unavailable, so changes last until you quit. Your existing file was left untouched.";
        glyph = "scratchpad";
    }
    bannerEl.replaceChildren();
    if (!text) {
        bannerEl.classList.add("hidden");
        return;
    }
    const label = document.createElement("span");
    label.textContent = text;
    bannerEl.append(icon(glyph, 14), label);
    bannerEl.classList.remove("hidden");
}

// --- Placements ---------------------------------------------------------------------

function getEffectivePlacementsForBreakpoint(bpName) {
    if (layout.placements[bpName]) {
        // Ensure every widget has a placement at this breakpoint.
        const existing = layout.placements[bpName];
        const byId = new Map(existing.map(p => [p.instanceId, p]));
        const bp = getBreakpointByName(bpName);
        const result = [];
        for (const p of existing) {
            if (layout.widgets.some(w => w.instanceId === p.instanceId))
                result.push({ ...p });
        }
        for (const w of layout.widgets) {
            if (!byId.has(w.instanceId)) {
                const def = getWidgetDef(w.widgetType);
                const eff = def ? effectiveSizesForBreakpoint(def.sizes, bp.cols) : [{ w: GRID_STEPS_PER_CELL * 2, h: GRID_STEPS_PER_CELL * 2 }];
                const defSize = eff[1] || eff[0];
                const slot = findDefaultSlot(result, defSize, bp.cols);
                if (slot)
                    result.push({ instanceId: w.instanceId, ...slot });
            }
        }
        return sortReadingOrder(result);
    }

    const wide = layout.placements.wide || [];
    const bp = getBreakpointByName(bpName);
    if (bpName === "wide")
        return sortReadingOrder(wide);
    return projectPlacements(wide, bp.cols);
}

function currentPlacements() {
    return getEffectivePlacementsForBreakpoint(currentBp.name);
}

function findDefaultSlot(placements, { w, h }, cols) {
    if (!placements.length)
        return { col: Math.round((cols - w) / 2), row: -Math.floor(h / 2), w, h };
    const firstRow = Math.min(...placements.map(p => p.row));
    return findFirstFreeSlot(placements, w, h, cols, firstRow);
}

function buildCommitPlacements(nextWidgets, editedBpName, editedBpPlacements) {
    const nextPlacements = {};
    const widgetIds = new Set(nextWidgets.map(w => w.instanceId));
    const { rowShift } = metrics();
    // When an edit vacates the top occupied row in a short window, keep the
    // drop and the other tiles at their previewed positions.
    if (rowShift > 0 && metrics(editedBpPlacements).rowShift < rowShift
        && editedBpPlacements.every(p => p.row + p.h + rowShift <= MAX_GRID_ROWS)) {
        editedBpPlacements = editedBpPlacements.map(p => ({ ...p, row: p.row + rowShift }));
    }

    // Always ensure canonical wide placements exist when widgets exist.
    if (editedBpName === "wide") {
        nextPlacements.wide = sortReadingOrder(editedBpPlacements);
    } else {
        const existingWide = (layout.placements.wide || []).filter(p => widgetIds.has(p.instanceId));
        const wideResult = [...existingWide];
        for (const w of nextWidgets) {
            if (!wideResult.some(p => p.instanceId === w.instanceId)) {
                const def = getWidgetDef(w.widgetType);
                const { cols } = getBreakpointByName("wide");
                const sizes = def ? effectiveSizesForBreakpoint(def.sizes, cols) : [{ w: 12, h: 8 }];
                const size = sizes[1] || sizes[0];
                const slot = findDefaultSlot(wideResult, size, cols);
                if (slot)
                    wideResult.push({ instanceId: w.instanceId, ...slot });
            }
        }
        nextPlacements.wide = sortReadingOrder(wideResult);
        nextPlacements[editedBpName] = sortReadingOrder(editedBpPlacements);
    }

    // Preserve any other previously saved breakpoints, removing deleted cards and placing added ones.
    for (const [bpName, list] of Object.entries(layout.placements)) {
        if (bpName === "wide" || bpName === editedBpName)
            continue;
        const bp = getBreakpointByName(bpName);
        const kept = list.filter(p => widgetIds.has(p.instanceId));
        for (const w of nextWidgets) {
            if (!kept.some(p => p.instanceId === w.instanceId)) {
                const def = getWidgetDef(w.widgetType);
                const eff = def ? effectiveSizesForBreakpoint(def.sizes, bp.cols) : [{ w: GRID_STEPS_PER_CELL * 2, h: GRID_STEPS_PER_CELL * 2 }];
                const s = eff[1] || eff[0];
                const slot = findDefaultSlot(kept, s, bp.cols);
                if (slot)
                    kept.push({ instanceId: w.instanceId, ...slot });
            }
        }
        nextPlacements[bpName] = sortReadingOrder(kept);
    }

    return nextPlacements;
}

function cloneSnapshot(src) {
    return JSON.parse(JSON.stringify(src));
}

async function commitLayoutChange(nextWidgets, nextPlacements, nextCompactMode, { recordUndo = true, announcement = "", focusInstanceId = null, pulseId = null } = {}) {
    const prevSnapshot = cloneSnapshot(layout);
    const opId = `op-${ctx.documentGeneration}-${Date.now()}-${opCounter++}`;
    setSaveStatus("Saving…");

    try {
        const res = await request("dashboard.commitLayout", {
            opId,
            expectedRevision: layout.layoutRevision,
            compactMode: nextCompactMode,
            widgets: nextWidgets,
            placements: nextPlacements,
        });

        layout = res.layout;
        if (res.storageStatus)
            ctx.storageStatus = res.storageStatus;

        // Both stacks hold this page's own acknowledged edits, oldest first. An edit that arrives from
        // another page clears them (see the layoutUpdated and conflict paths), so every entry still
        // describes the layout it was recorded against.
        if (recordUndo && !res.duplicate) {
            undoStack.push({ before: prevSnapshot, after: cloneSnapshot(layout) });
            if (undoStack.length > MAX_UNDO)
                undoStack.shift();
            redoStack = [];
        }

        renderGrid();
        updateToolbarState();
        refreshShelf();
        setSaveStatus(ctx.isEphemeral ? "Session only" : "Saved", true);
        if (announcement)
            announce(announcement);
        restoreFocusAfterCommit(focusInstanceId);
        if (pulseId) {
            const landed = currentPlacements().find(p => p.instanceId === pulseId);
            if (landed)
                dots.pulse(landed);
        }
        return true;
    } catch (err) {
        if (err.conflict && err.payload?.layout) {
            layout = err.payload.layout;
            undoStack = [];
            redoStack = [];
            renderGrid();
            updateToolbarState();
            setSaveStatus("Updated from another tab", true);
            announce("Layout refreshed after an edit in another tab.");
            return false;
        }
        renderGrid();
        updateToolbarState();
        setSaveStatus("Couldn’t save");
        announce(err.message || "Couldn’t save dashboard layout.");
        return false;
    }
}

function restoreFocusAfterCommit(focusInstanceId) {
    if (isEditing && focusInstanceId) {
        const target = mountedCards.get(focusInstanceId)?.chrome.dragBtn;
        if (target) {
            target.focus({ preventScroll: true });
            return;
        }
    }
    if (layout.widgets.length === 0 && document.activeElement === document.body)
        (isEditing ? btnAddWidget : emptyAddBtn).focus();
}

// --- Geometry -----------------------------------------------------------------------

function isRtl() {
    return getComputedStyle(boardEl).direction === "rtl";
}

function metrics(placements = currentPlacements()) {
    const width = boardEl.clientWidth;
    const { cols, gap, rowHeight } = currentBp;
    const unitY = (rowHeight + gap) / GRID_STEPS_PER_CELL;
    const viewportHeight = rootEl.clientHeight;
    const centeredOrigin = (viewportHeight + gap) / 2;
    const firstRow = Math.min(0, ...placements.map(p => p.row));
    // Keep the topmost tile reachable in a short window. Shift by whole rows
    // so that the viewport center remains a reachable snap point.
    const firstVisibleRow = Math.ceil(-centeredOrigin / unitY);
    const rowShift = Math.max(0, firstVisibleRow - firstRow);
    const originY = centeredOrigin + rowShift * unitY;
    return {
        width, cols, gap, unitY, originY, viewportHeight, rowShift,
        unitX: (width - TILE_INLINE_INSET * 2 + gap) / cols,
        inlineInset: TILE_INLINE_INSET,
        minRow: Math.max(MIN_GRID_ROW, firstVisibleRow - rowShift),
        rtl: isRtl(),
    };
}

function rectFor(placement, m) {
    const w = placement.w * m.unitX - m.gap;
    const h = placement.h * m.unitY - m.gap;
    let x = m.inlineInset + placement.col * m.unitX;
    if (m.rtl)
        x = m.width - x - w;
    return { x, y: m.originY + placement.row * m.unitY, w, h };
}

function applyBoardSize(placements, m) {
    const lastRow = Math.max(m.minRow, ...placements.map(p => p.row + p.h));
    // Leave room below occupied rows for scrolling while dragging, and fill
    // the window even when there are no tiles.
    const endRow = Math.min(MAX_GRID_ROWS, lastRow + (isEditing ? EXTRA_EDIT_ROWS : 0));
    const bottomClearance = !isEditing && !bannerEl.classList.contains("hidden")
        ? Math.max(TILE_BOTTOM_CLEARANCE, m.viewportHeight - footerEl.getBoundingClientRect().top + 8)
        : TILE_BOTTOM_CLEARANCE;
    const height = Math.max(
        m.viewportHeight,
        m.originY + endRow * m.unitY - m.gap,
        m.originY + lastRow * m.unitY - m.gap + bottomClearance,
    );
    boardEl.style.height = `${height}px`;
    boardEl.style.setProperty("--tile-gap", `${m.gap}px`);
    dots.setGeometry({ ...m, height });
}

function positionTile(section, placement, m) {
    const rect = rectFor(placement, m);
    section.style.width = `${rect.w}px`;
    section.style.height = `${rect.h}px`;
    section.style.translate = `${rect.x}px ${rect.y}px`;
}

function positionLanding(placement, m) {
    if (!placement) {
        landingEl.classList.add("hidden");
        return;
    }
    const rect = rectFor(placement, m);
    landingEl.style.width = `${rect.w}px`;
    landingEl.style.height = `${rect.h}px`;
    landingEl.style.translate = `${rect.x}px ${rect.y}px`;
    landingEl.classList.remove("hidden");
}

function withoutMotion(fn) {
    boardEl.classList.add("no-motion");
    fn();
    // Two frames: one for the style change to land, one before transitions resume.
    requestAnimationFrame(() => requestAnimationFrame(() => boardEl.classList.remove("no-motion")));
}

function cellFromPoint(clientX, clientY, m) {
    const rect = boardEl.getBoundingClientRect();
    let x = clientX - rect.left;
    if (m.rtl)
        x = m.width - x;
    return {
        col: Math.floor((x - m.inlineInset + m.gap / 2) / m.unitX),
        row: Math.floor((clientY - rect.top - m.originY + m.gap / 2) / m.unitY),
    };
}

// --- Tiles --------------------------------------------------------------------------

function unmountCard(instanceId, animate = false) {
    const entry = mountedCards.get(instanceId);
    if (!entry)
        return;
    mountedCards.delete(instanceId);
    try {
        entry.controller?.unmount?.();
    } catch (e) {
        console.error("Widget unmount error:", e);
    }
    const section = entry.chrome.section;
    if (animate && !reducedMotion.matches) {
        section.classList.add("is-leaving");
        section.removeAttribute("role");
        setTimeout(() => section.remove(), 220);
    } else {
        section.remove();
    }
}

function mountWidgetIntoCard(entry) {
    const { instance, chrome } = entry;
    const def = getWidgetDef(instance.widgetType);
    const placement = currentPlacements().find(p => p.instanceId === instance.instanceId);
    const tier = computeCardTier(placement || { w: 12, h: 8 }, def, currentBp.cols);

    const mountSeq = ++entry.mountSeq;
    chrome.body.replaceChildren();

    entry.mountPromise = loadWidgetModule(instance.widgetType)
        .then(mod => {
            if (entry.mountSeq !== mountSeq || !mountedCards.has(instance.instanceId))
                return;
            chrome.body.replaceChildren();
            const widgetApi = {
                instanceId: instance.instanceId,
                widgetType: instance.widgetType,
                config: instance.config || {},
                context: ctx,
                tier,
                request,
                onEvent: onDashboardEvent,
                setMeta: text => {
                    chrome.metaEl.textContent = text || "";
                },
                openUrl: (url, newTab = false) => request("navigation.openUrl", { url, newTab }),
                openSettings: () => openConfigureDialog(instance.instanceId),
                updateConfig: async patch => {
                    const nextConfig = { ...(entry.instance.config || {}), ...patch };
                    const nextWidgets = layout.widgets.map(w =>
                        w.instanceId === instance.instanceId ? { ...w, config: nextConfig } : w
                    );
                    const nextPlacements = buildCommitPlacements(nextWidgets, currentBp.name, currentPlacements());
                    return commitLayoutChange(nextWidgets, nextPlacements, layout.compactMode, { recordUndo: false });
                },
                reportError: errMsg => {
                    renderCardError(
                        chrome.body,
                        errMsg,
                        () => mountWidgetIntoCard(entry),
                        () => handleRemoveWidget(instance.instanceId)
                    );
                },
                announce,
            };

            const ctrl = mod.mount(chrome.body, widgetApi);
            entry.controller = ctrl || {};
            entry.hasConfigure = typeof entry.controller.renderConfig === "function";
        })
        .catch(err => {
            if (entry.mountSeq !== mountSeq || !mountedCards.has(instance.instanceId))
                return;
            renderCardError(
                chrome.body,
                err?.message || "This widget failed to load.",
                () => mountWidgetIntoCard(entry),
                () => handleRemoveWidget(instance.instanceId)
            );
        });
}

const cardHandlers = {
    isEditing: () => isEditing,
    isSwapMode: () => Boolean(swapSourceId),
    cardCount: () => layout.widgets.length,
    cols: () => currentBp.cols,
    getPlacement: id => currentPlacements().find(p => p.instanceId === id) || null,
    hasConfigure: id => Boolean(mountedCards.get(id)?.hasConfigure),
    onActivate: id => {
        if (swapSourceId) {
            completeSwap(id);
            return;
        }
        selectTile(id);
    },
    onFocusHandle: id => {
        if (isEditing && !swapSourceId)
            selectTile(id);
    },
    onStartSwap: id => {
        if (!isEditing)
            setEditing(true);
        swapSourceId = id;
        selectTile(null);
        updateToolbarState();
        refreshTileStates();
        announce("Choose another tile to swap places with, or press Escape to cancel.");
    },
    onResizeTo: (id, w, h) => handleResizeTo(id, w, h),
    onOpenReplacePicker: id => openReplacePicker(id),
    isCustomizeHidden: () => customizeHidden,
    onShowCustomizeButton: () => setCustomizeButton("shown"),
    onOpenConfigure: id => openConfigureDialog(id),
    onRemove: id => handleRemoveWidget(id),
    onKeyboardMove: (id, dx, dy) => handleKeyboardMove(id, dx, dy),
    onKeyboardResize: (id, dx, dy) => handleKeyboardResize(id, dx, dy),
    onBeginGesture: (id, mode, point) => beginPointerGesture(id, mode, point),
    onUpdateGesture: (id, mode, point) => updatePointerGesture(point),
    onCommitGesture: () => commitPointerGesture(),
    onCancelGesture: () => cancelPointerGesture(),
};

function renderGrid() {
    const placements = currentPlacements();
    const widgetById = new Map(layout.widgets.map(w => [w.instanceId, w]));

    const hasCards = layout.widgets.length > 0;
    emptyEl.classList.toggle("hidden", hasCards);
    rootEl.classList.toggle("is-empty", !hasCards);

    for (const id of [...mountedCards.keys()]) {
        if (!widgetById.has(id))
            unmountCard(id, hasRenderedOnce);
    }

    const m = metrics(placements);
    applyBoardSize(placements, m);

    const firstRender = !hasRenderedOnce;
    const ordered = sortReadingOrder(placements);
    for (const p of ordered) {
        const inst = widgetById.get(p.instanceId);
        if (!inst)
            continue;
        const def = getWidgetDef(inst.widgetType);
        let entry = mountedCards.get(inst.instanceId);
        if (!entry) {
            const chrome = createCardElement({ instance: inst, def, handlers: cardHandlers });
            entry = { instance: inst, chrome, controller: null, hasConfigure: false, mountSeq: 0 };
            mountedCards.set(inst.instanceId, entry);
            applyPlacementData(chrome.section, p, def, currentBp.cols);
            positionTile(chrome.section, p, m);
            if (!firstRender && !reducedMotion.matches) {
                chrome.section.classList.add("is-entering");
                chrome.section.addEventListener("animationend", () => chrome.section.classList.remove("is-entering"), { once: true });
            }
            gridEl.append(chrome.section);
            mountWidgetIntoCard(entry);
        } else {
            const prevConfig = JSON.stringify(entry.instance.config || {});
            const nextConfig = JSON.stringify(inst.config || {});
            const prevTier = entry.chrome.section.dataset.tier;
            entry.instance = inst;
            applyPlacementData(entry.chrome.section, p, def, currentBp.cols);
            positionTile(entry.chrome.section, p, m);
            const nextTier = entry.chrome.section.dataset.tier;
            if (prevConfig !== nextConfig || prevTier !== nextTier)
                entry.controller?.onUpdate?.({ config: inst.config || {}, tier: nextTier });
        }
    }

    hasRenderedOnce = true;
    refreshTileStates();
    updateDotVisibility();
    scheduleDomOrderSync(firstRender ? 0 : SETTLE_MS);
}

// Tiles are placed with transforms, so their order in the document is kept in step with
// reading order separately, once any movement has finished.
function scheduleDomOrderSync(delay) {
    clearTimeout(domOrderTimer);
    domOrderTimer = setTimeout(() => {
        if (gestureState)
            return;
        const desired = sortReadingOrder(currentPlacements())
            .map(p => mountedCards.get(p.instanceId)?.chrome.section)
            .filter(Boolean);
        const present = [...gridEl.children].filter(el => desired.includes(el));
        if (desired.every((el, i) => present[i] === el))
            return;
        const focused = document.activeElement;
        for (const el of desired)
            gridEl.append(el);
        if (focused && focused !== document.body && document.contains(focused) && document.activeElement !== focused)
            focused.focus({ preventScroll: true });
    }, delay);
}

function previewPlacements(placements, skipId = null) {
    const m = gestureState?.m || metrics();
    for (const p of placements) {
        if (p.instanceId === skipId)
            continue;
        const entry = mountedCards.get(p.instanceId);
        if (entry)
            positionTile(entry.chrome.section, p, m);
    }
}

function refreshTileStates() {
    for (const [id, entry] of mountedCards) {
        const el = entry.chrome.section;
        el.classList.toggle("is-selected", isEditing && selectedId === id && !swapSourceId);
        el.classList.toggle("is-swap-source", swapSourceId === id);
        el.classList.toggle("is-swap-target", Boolean(swapSourceId && swapSourceId !== id));
        entry.chrome.face.inert = isEditing;
        entry.chrome.dragBtn.tabIndex = isEditing ? 0 : -1;
        entry.chrome.removeBtn.tabIndex = isEditing ? 0 : -1;
    }
}

function selectTile(id) {
    if (selectedId === id)
        return;
    selectedId = id;
    clearHoverCell();
    refreshTileStates();
    updateToolbarState();
}

function updateDotVisibility(origin = null) {
    const shouldShow = isEditing || layout.widgets.length === 0;
    if (shouldShow)
        dots.show(origin);
    else
        dots.hide();
}

function updateToolbarState() {
    rootEl.classList.toggle("is-editing", isEditing);
    gridEl.classList.toggle("is-editing", isEditing);
    editToolbarEl.classList.toggle("hidden", !isEditing);
    customizeWrapEl.classList.toggle("hidden", isEditing || customizeHidden);
    gridWrapEl.tabIndex = customizeHidden && !isEditing ? 0 : -1;
    if (gridWrapEl.tabIndex === 0)
        gridWrapEl.setAttribute("aria-description", "Press Enter for dashboard options.");
    else
        gridWrapEl.removeAttribute("aria-description");
    btnCompact.setAttribute("aria-pressed", layout.compactMode ? "true" : "false");
    btnUndo.disabled = undoStack.length === 0;
    btnRedo.disabled = redoStack.length === 0;

    let hint = "";
    let showCancel = false;
    if (swapSourceId) {
        const src = layout.widgets.find(w => w.instanceId === swapSourceId);
        const def = src ? getWidgetDef(src.widgetType) : null;
        hint = `Choose a tile to swap with ${def ? getWidgetShortTitle(def) : "this tile"}`;
        showCancel = true;
    } else if (isEditing && selectedId && !picker.isOpen()) {
        hint = "Drag it, or click an open spot to move it there";
    }
    swapTextEl.textContent = hint;
    swapCancelBtn.classList.toggle("hidden", !showCancel);
    swapBannerEl.classList.toggle("hidden", !hint);
}

function setEditing(next, origin = null) {
    if (isEditing === next)
        return;
    if (!next) {
        clearSwapMode();
        picker.close();
        closeCardMenus(false);
    }
    isEditing = next;
    selectedId = null;
    clearHoverCell();
    updateToolbarState();
    refreshTileStates();
    const m = metrics();
    applyBoardSize(currentPlacements(), m);
    updateDotVisibility(origin);
    announce(next ? "Editing the dashboard. Tiles snap to the dot grid." : "Finished editing the dashboard.");
}

function clearSwapMode() {
    if (!swapSourceId)
        return;
    swapSourceId = null;
    updateToolbarState();
    refreshTileStates();
}

async function completeSwap(targetId) {
    if (!swapSourceId || swapSourceId === targetId) {
        clearSwapMode();
        return;
    }
    const srcId = swapSourceId;
    clearSwapMode();
    const cur = currentPlacements();
    const swapped = swapPlacements(cur, srcId, targetId, currentBp.cols, layout.compactMode, metrics(cur).minRow);
    if (!swapped.ok) {
        announce(swapped.error);
        setSaveStatus(swapped.error, true);
        return;
    }
    const nextPlacements = buildCommitPlacements(layout.widgets, currentBp.name, swapped.placements);
    await commitLayoutChange(layout.widgets, nextPlacements, layout.compactMode, {
        announcement: "Swapped the two tiles.",
        focusInstanceId: srcId,
        pulseId: srcId,
    });
}

// --- Commands -----------------------------------------------------------------------

function newInstance(def, prefix = "w") {
    return {
        instanceId: `${prefix}-${def.id}-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 6)}`,
        widgetType: def.id,
        configVersion: 1,
        config: def.defaultConfig ? def.defaultConfig() : {},
    };
}

function defaultSizeFor(def, preferred = null) {
    const effSizes = effectiveSizesForBreakpoint(def.sizes, currentBp.cols);
    if (preferred) {
        const match = effSizes.find(s => s.tier === preferred);
        if (match)
            return match;
    }
    return effSizes[1] || effSizes[0];
}

async function handleAddWidget(def, { tier = null, placement = null } = {}) {
    if (layout.widgets.length >= (ctx.maxCards || 60)) {
        const message = `The board is full (${ctx.maxCards || 60} tiles).`;
        announce(message);
        setSaveStatus(message, true);
        return false;
    }
    const inst = newInstance(def);
    const nextWidgets = [...layout.widgets, inst];
    const cur = currentPlacements();
    const m = metrics(cur);
    let nextCur;
    let landed;
    if (placement) {
        const resolved = resolveLayout(
            [...cur, { instanceId: inst.instanceId, col: placement.col, row: placement.row, w: placement.w, h: placement.h }],
            [inst.instanceId],
            currentBp.cols,
            layout.compactMode,
            m.minRow
        );
        if (!resolved.ok) {
            announce(resolved.error);
            return false;
        }
        nextCur = resolved.placements;
        landed = nextCur.find(p => p.instanceId === inst.instanceId);
    } else {
        const size = defaultSizeFor(def, tier);
        const slot = findDefaultSlot(cur, size, currentBp.cols);
        if (!slot) {
            announce("There is no room left on the board.");
            return false;
        }
        landed = { instanceId: inst.instanceId, ...slot };
        nextCur = [...cur, landed];
    }
    const nextPlacements = buildCommitPlacements(nextWidgets, currentBp.name, nextCur);
    const ok = await commitLayoutChange(nextWidgets, nextPlacements, layout.compactMode, {
        announcement: `Added ${def.title} at column ${landed.col + 1}, row ${landed.row - m.minRow + 1}.`,
        pulseId: inst.instanceId,
    });
    if (ok && !placement) {
        const section = mountedCards.get(inst.instanceId)?.chrome.section;
        section?.scrollIntoView({ block: "nearest", behavior: reducedMotion.matches ? "auto" : "smooth" });
    }
    if (ok && def.id === "image")
        await openNewImageSettings(inst.instanceId);
    return ok;
}

async function handleReplaceWidget(newDef, oldInstanceId) {
    const cur = currentPlacements();
    const oldPlacement = cur.find(p => p.instanceId === oldInstanceId);
    if (!oldPlacement)
        return;
    const inst = newInstance(newDef);
    const nextWidgets = layout.widgets.map(w => (w.instanceId === oldInstanceId ? inst : w));
    const effSizes = effectiveSizesForBreakpoint(newDef.sizes, currentBp.cols);
    const snapped = snapResizeSize(oldPlacement.w, oldPlacement.h, effSizes);
    const next = cur.map(p =>
        p.instanceId === oldInstanceId
            ? {
                instanceId: inst.instanceId,
                col: Math.min(p.col, currentBp.cols - snapped.w),
                row: p.row,
                w: snapped.w,
                h: snapped.h,
            }
            : p
    );
    const resolved = resolveLayout(next, [inst.instanceId], currentBp.cols, layout.compactMode, metrics(cur).minRow);
    if (!resolved.ok) {
        announce(resolved.error);
        return;
    }
    const nextPlacements = buildCommitPlacements(nextWidgets, currentBp.name, resolved.placements);
    const ok = await commitLayoutChange(nextWidgets, nextPlacements, layout.compactMode, {
        announcement: `Replaced the tile with ${newDef.title}.`,
        focusInstanceId: inst.instanceId,
        pulseId: inst.instanceId,
    });
    if (ok && newDef.id === "image")
        await openNewImageSettings(inst.instanceId);
}

async function handleRemoveWidget(instanceId) {
    const cur = currentPlacements();
    const idx = cur.findIndex(p => p.instanceId === instanceId);
    const nextFocusId = cur[idx + 1]?.instanceId || cur[idx - 1]?.instanceId || null;
    const hadFocus = mountedCards.get(instanceId)?.chrome.section.contains(document.activeElement);

    const nextWidgets = layout.widgets.filter(w => w.instanceId !== instanceId);
    const remaining = cur.filter(p => p.instanceId !== instanceId);
    const resolved = layout.compactMode
        ? resolveLayout(remaining, [], currentBp.cols, true, metrics(cur).minRow)
        : { ok: true, placements: remaining };
    const nextPlacements = buildCommitPlacements(nextWidgets, currentBp.name, resolved.placements);
    const def = getWidgetDef(layout.widgets.find(w => w.instanceId === instanceId)?.widgetType);
    if (selectedId === instanceId)
        selectedId = null;
    await commitLayoutChange(nextWidgets, nextPlacements, layout.compactMode, {
        announcement: `Removed ${def ? def.title : "tile"}.`,
        focusInstanceId: hadFocus ? nextFocusId : null,
    });
}

async function commitTilePlacement(instanceId, candidate, announcement) {
    const cur = currentPlacements();
    const next = cur.map(p => (p.instanceId === instanceId ? { ...p, ...candidate, instanceId } : p));
    const resolved = resolveLayout(next, [instanceId], currentBp.cols, layout.compactMode, metrics(cur).minRow);
    if (!resolved.ok) {
        announce(resolved.error);
        setSaveStatus(resolved.error, true);
        return false;
    }
    const nextPlacements = buildCommitPlacements(layout.widgets, currentBp.name, resolved.placements);
    return commitLayoutChange(layout.widgets, nextPlacements, layout.compactMode, {
        announcement,
        focusInstanceId: instanceId,
        pulseId: instanceId,
    });
}

async function handleResizeTo(instanceId, targetW, targetH) {
    const target = currentPlacements().find(p => p.instanceId === instanceId);
    if (!target)
        return;
    await commitTilePlacement(
        instanceId,
        { col: Math.min(target.col, currentBp.cols - targetW), row: target.row, w: targetW, h: targetH },
        `Resized to ${formatGridSize({ w: targetW, h: targetH }, " by ")}.`
    );
}

async function handleKeyboardMove(instanceId, dx, dy) {
    const placements = currentPlacements();
    const target = placements.find(p => p.instanceId === instanceId);
    if (!target)
        return;
    const { minRow } = metrics(placements);
    const col = Math.max(0, Math.min(target.col + dx, currentBp.cols - target.w));
    const row = Math.max(minRow, Math.min(target.row + dy, MAX_GRID_ROWS - target.h));
    if (col === target.col && row === target.row)
        return;
    await commitTilePlacement(instanceId, { col, row, w: target.w, h: target.h }, `Moved to column ${col + 1}, row ${row - minRow + 1}.`);
}

async function handleKeyboardResize(instanceId, dx, dy) {
    const inst = layout.widgets.find(w => w.instanceId === instanceId);
    const def = inst ? getWidgetDef(inst.widgetType) : null;
    const target = currentPlacements().find(p => p.instanceId === instanceId);
    if (!def || !target)
        return;
    const valid = effectiveSizesForBreakpoint(def.sizes, currentBp.cols);
    const limits = resizeLimits(target, valid, currentBp.cols);
    const next = snapResizePlacement(target, target.w + dx * GRID_RESIZE_STEP, target.h + dy * GRID_RESIZE_STEP, limits);
    if (next.w === target.w && next.h === target.h)
        return;
    await handleResizeTo(instanceId, next.w, next.h);
}

// --- Drag, resize and drag-in ---------------------------------------------------------
//
// A gesture works from a snapshot taken when it starts. The tile under the pointer moves
// freely; everything else is laid out by the solver around the cell it would land on, and
// the dot field lights that landing spot. Nothing is written until the drop.

function beginPointerGesture(instanceId, mode, point) {
    const base = currentPlacements();
    const start = base.find(p => p.instanceId === instanceId);
    const entry = mountedCards.get(instanceId);
    if (!start || !entry)
        return;
    closeCardMenus(false);
    clearHoverCell();
    selectedId = instanceId;
    refreshTileStates();

    const m = metrics(base);
    const boardRect = boardEl.getBoundingClientRect();
    const startRect = rectFor(start, m);
    const inst = layout.widgets.find(w => w.instanceId === instanceId);
    const def = inst ? getWidgetDef(inst.widgetType) : null;

    const sizes = def ? effectiveSizesForBreakpoint(def.sizes, m.cols) : [];

    gestureState = {
        mode,
        instanceId,
        bpName: currentBp.name,
        base,
        start,
        m,
        def,
        section: entry.chrome.section,
        sizeBadge: entry.chrome.sizeBadge,
        startRect,
        resizeLimits: resizeLimits(start, sizes, m.cols),
        grabDX: point.clientX - boardRect.left - startRect.x,
        grabDY: point.clientY - boardRect.top - startRect.y,
        // For a resize, how far the pointer sits from the corner being dragged.
        cornerDX: m.rtl
            ? point.clientX - boardRect.left - startRect.x
            : startRect.x + startRect.w - (point.clientX - boardRect.left),
        cornerDY: startRect.y + startRect.h - (point.clientY - boardRect.top),
        candidate: { ...start },
        preview: base,
        lastPoint: point,
        lastX: point.clientX,
        tilt: 0,
    };

    entry.chrome.section.classList.add(mode === "resize" ? "is-resizing" : "is-dragging");
    rootEl.classList.add("is-gesturing");
    dots.setFootprint(start);
    positionLanding(start, m);
    updateAlignmentGuides(startRect);
    if (mode === "resize")
        updateSizeBadge(start);
    startAutoScroll();
}

function candidateForMove(x, y, w, h, m) {
    const wPx = w * m.unitX - m.gap;
    const inlineX = (m.rtl ? m.width - x - wPx : x) - m.inlineInset;
    return {
        col: Math.max(0, Math.min(Math.round(inlineX / m.unitX), m.cols - w)),
        row: Math.max(m.minRow, Math.min(Math.round((y - m.originY) / m.unitY), MAX_GRID_ROWS - h)),
        w,
        h,
    };
}

function resolvePreview(candidate) {
    const gs = gestureState;
    const id = gs.instanceId;
    const others = gs.base.filter(p => p.instanceId !== id);

    // Dropping a tile exactly onto another of the same size trades their places.
    if (gs.mode === "move") {
        const twin = others.find(p => p.col === candidate.col && p.row === candidate.row && p.w === candidate.w && p.h === candidate.h);
        if (twin) {
            const swapped = gs.base.map(p => {
                if (p.instanceId === id)
                    return { ...p, col: candidate.col, row: candidate.row };
                if (p.instanceId === twin.instanceId)
                    return { ...p, col: gs.start.col, row: gs.start.row };
                return { ...p };
            });
            return { ok: true, placements: sortReadingOrder(swapped) };
        }
    }

    const next = [...others, { instanceId: id, ...candidate }];
    return resolveLayout(next, [id], gs.m.cols, layout.compactMode, gs.m.minRow);
}

function updateSizeBadge(candidate) {
    const gs = gestureState;
    if (!gs?.sizeBadge)
        return;
    const tier = computeCardTier(candidate, gs.def, gs.m.cols);
    gs.sizeBadge.textContent = `${SIZE_NAMES[tier] || "Size"} · ${formatGridSize(candidate)}`;
    gs.sizeBadge.classList.remove("hidden");
}

function applyCandidate(candidate) {
    const gs = gestureState;
    const same = gs.candidate
        && candidate
        && gs.candidate.col === candidate.col && gs.candidate.row === candidate.row
        && gs.candidate.w === candidate.w && gs.candidate.h === candidate.h;
    if (same || (!candidate && !gs.candidate))
        return;

    if (!candidate) {
        gs.candidate = null;
        gs.preview = gs.base;
        previewPlacements(gs.base, gs.instanceId);
        dots.setFootprint(null);
        positionLanding(null, gs.m);
        return;
    }

    const resolved = resolvePreview(candidate);
    if (!resolved.ok)
        return;
    gs.candidate = candidate;
    gs.preview = resolved.placements;
    applyBoardSize(resolved.placements, gs.m);
    previewPlacements(resolved.placements, gs.instanceId);
    dots.setFootprint(candidate);
    positionLanding(candidate, gs.m);
    if (gs.mode === "resize")
        updateSizeBadge(candidate);
}

function updateAlignmentGuides(inHand) {
    const gs = gestureState;
    guideX.hidden = guideY.hidden = true;
    gs.centeredScrollDelta = 0;
    if (!gs.candidate)
        return inHand;

    const { m } = gs;
    const landing = rectFor(gs.candidate, m);
    const board = boardEl.getBoundingClientRect();
    const viewportWidth = rootEl.clientWidth;
    const viewportHeight = rootEl.clientHeight;
    const visibleTop = Math.max(0, -board.top);
    const visibleBottom = Math.min(board.height, viewportHeight - board.top);
    const centerX = landing.x + landing.w / 2;
    const centerY = landing.y + landing.h / 2;
    const viewportCenterY = viewportHeight / 2 - board.top;
    const scrollDelta = centerY - viewportCenterY;
    const viewportX = Math.abs(centerX - (viewportWidth / 2 - board.left)) <= 0.75;
    // Scrolling can leave the viewport center between two rows. Align the
    // preview here, then bring the drop position to the same line on release.
    const centeredScroll = window.scrollY + scrollDelta;
    const viewportY = Math.abs(scrollDelta) <= m.unitY / 2
        && centeredScroll >= -0.5 && centeredScroll <= rootEl.scrollHeight - viewportHeight + 0.5;
    const others = gs.preview.filter(p => p.instanceId !== gs.instanceId).map(p => rectFor(p, m));
    const visiblePeers = others.filter(p => p.y < visibleBottom && p.y + p.h > visibleTop);
    const peerX = visiblePeers.find(p => Math.abs(p.x + p.w / 2 - centerX) <= 0.75);
    const peerY = visiblePeers.find(p => Math.abs(p.y + p.h / 2 - centerY) <= 0.75);

    const aligned = { ...inHand };
    if ((viewportX || peerX)
        && Math.abs(inHand.x + inHand.w / 2 - centerX) <= Math.min(10, m.unitX / 2)
        && visibleBottom > visibleTop) {
        const top = viewportX ? visibleTop : Math.max(visibleTop, Math.min(landing.y, peerX.y) - 8);
        const bottom = viewportX ? visibleBottom : Math.min(visibleBottom, Math.max(landing.y + landing.h, peerX.y + peerX.h) + 8);
        if (bottom > top) {
            guideX.classList.toggle("is-viewport", viewportX);
            guideX.style.left = `${centerX}px`;
            guideX.style.top = `${top}px`;
            guideX.style.height = `${bottom - top}px`;
            guideX.hidden = false;
            aligned.x = landing.x;
            aligned.w = landing.w;
        }
    }
    if ((viewportY || peerY)
        && Math.abs(inHand.y + inHand.h / 2 - centerY) <= Math.min(10, m.unitY / 2)
        && centerY >= visibleTop && centerY <= visibleBottom) {
        const left = viewportY ? 0 : Math.max(0, Math.min(landing.x, peerY.x) - 8);
        const right = viewportY ? m.width : Math.min(m.width, Math.max(landing.x + landing.w, peerY.x + peerY.w) + 8);
        guideY.classList.toggle("is-viewport", viewportY);
        guideY.style.left = `${left}px`;
        guideY.style.top = `${viewportY ? viewportCenterY : centerY}px`;
        guideY.style.width = `${right - left}px`;
        guideY.hidden = false;
        aligned.y = landing.y - (viewportY ? scrollDelta : 0);
        aligned.h = landing.h;
        if (viewportY)
            gs.centeredScrollDelta = scrollDelta;
    }
    return aligned;
}

function updatePointerGesture(point) {
    const gs = gestureState;
    if (!gs)
        return;
    gs.lastPoint = point;
    const m = gs.m;
    const boardRect = boardEl.getBoundingClientRect();
    const px = point.clientX - boardRect.left;
    const py = point.clientY - boardRect.top;

    if (gs.mode === "resize") {
        const { minW, minH, maxW, maxH } = gs.resizeLimits;
        const desiredWidth = m.rtl
            ? gs.startRect.x + gs.startRect.w - (px - gs.cornerDX)
            : px + gs.cornerDX - gs.startRect.x;
        const wPx = Math.max(minW * m.unitX - m.gap, Math.min(maxW * m.unitX - m.gap, desiredWidth));
        const hPx = Math.max(minH * m.unitY - m.gap, Math.min(maxH * m.unitY - m.gap, py + gs.cornerDY - gs.startRect.y));
        const xPx = m.rtl ? gs.startRect.x + gs.startRect.w - wPx : gs.startRect.x;

        applyCandidate(snapResizePlacement(gs.start, (wPx + m.gap) / m.unitX, (hPx + m.gap) / m.unitY, gs.resizeLimits));
        const aligned = updateAlignmentGuides({ x: xPx, y: gs.startRect.y, w: wPx, h: hPx });
        gs.section.style.width = `${aligned.w}px`;
        gs.section.style.height = `${aligned.h}px`;
        gs.section.style.translate = `${aligned.x}px ${aligned.y}px`;
        return;
    }

    // Move and drag-in: the tile follows the pointer.
    const w = gs.start.w;
    const h = gs.start.h;
    const wPx = w * m.unitX - m.gap;
    const hPx = h * m.unitY - m.gap;
    const x = Math.max(-24, Math.min(px - gs.grabDX, m.width - wPx + 24));
    const y = Math.max(-24, py - gs.grabDY);
    let outside = false;
    if (gs.mode === "add") {
        // The ducked shelf still shows a sliver at the bottom; letting go there puts the widget back.
        const overShelf = picker.isOpen() && point.clientY > window.innerHeight - 46;
        outside = point.clientY < boardRect.top - 60 || overShelf;
        gs.section.classList.toggle("is-homeless", outside);
    }
    applyCandidate(outside ? null : candidateForMove(x, y, w, h, m));
    const aligned = updateAlignmentGuides({ x, y, w: wPx, h: hPx });
    gs.section.style.translate = `${aligned.x}px ${aligned.y}px`;
}

function endGestureVisuals() {
    const gs = gestureState;
    stopAutoScroll();
    rootEl.classList.remove("is-gesturing");
    dots.setFootprint(null);
    positionLanding(null, gs.m);
    guideX.hidden = guideY.hidden = true;
    gs.sizeBadge?.classList.add("hidden");
    gs.section.style.rotate = "";
    gs.section.classList.remove("is-dragging", "is-resizing", "is-homeless");
}

function settle(section) {
    section.classList.add("is-settling");
    setTimeout(() => section.classList.remove("is-settling"), SETTLE_MS);
}

async function commitPointerGesture() {
    const gs = gestureState;
    if (!gs)
        return;
    // Keep the snapped tile on the viewport guide when the page was scrolled
    // between grid rows. Native scrolling rounds to whole CSS pixels.
    if (Math.abs(gs.centeredScrollDelta) >= 0.5)
        window.scrollBy(0, gs.centeredScrollDelta);
    endGestureVisuals();
    gestureState = null;

    if (gs.mode === "add") {
        gs.section.remove();
        rootEl.classList.remove("shelf-peeking");
        if (!gs.candidate) {
            renderGrid();
            announce("Cancelled adding the widget.");
            return;
        }
        await handleAddWidget(gs.def, { placement: gs.candidate });
        return;
    }

    const unchanged = gs.candidate
        && gs.candidate.col === gs.start.col && gs.candidate.row === gs.start.row
        && gs.candidate.w === gs.start.w && gs.candidate.h === gs.start.h;
    settle(gs.section);
    if (unchanged || !gs.candidate) {
        renderGrid();
        return;
    }

    // Let the tile fall into its slot right away; the commit confirms it.
    positionTile(gs.section, gs.candidate, gs.m);
    const nextPlacements = buildCommitPlacements(layout.widgets, gs.bpName, gs.preview);
    const landed = gs.candidate;
    const message = gs.mode === "move"
        ? `Moved to column ${landed.col + 1}, row ${landed.row - gs.m.minRow + 1}.`
        : `Resized to ${formatGridSize(landed, " by ")}.`;
    await commitLayoutChange(layout.widgets, nextPlacements, layout.compactMode, {
        announcement: message,
        focusInstanceId: gs.instanceId,
        pulseId: gs.instanceId,
    });
}

function cancelPointerGesture() {
    const gs = gestureState;
    if (!gs)
        return;
    endGestureVisuals();
    gestureState = null;
    if (gs.mode === "add") {
        gs.section.remove();
        rootEl.classList.remove("shelf-peeking");
    } else {
        settle(gs.section);
    }
    renderGrid();
    announce("Cancelled the change.");
}

// While a tile is held near the top or bottom edge of the window, the page scrolls under it.
let autoScrollFrame = 0;

function startAutoScroll() {
    const EDGE = 64;
    const tick = () => {
        autoScrollFrame = requestAnimationFrame(tick);
        const gs = gestureState;
        if (!gs)
            return;

        // A tile in hand leans into the direction it is travelling and rights itself when it stops.
        if (gs.mode !== "resize" && !reducedMotion.matches) {
            const velocity = gs.lastPoint.clientX - gs.lastX;
            gs.lastX = gs.lastPoint.clientX;
            gs.tilt += (Math.max(-2.6, Math.min(2.6, velocity * 0.3)) - gs.tilt) * 0.16;
            if (Math.abs(gs.tilt) < 0.02)
                gs.tilt = 0;
            gs.section.style.rotate = `${gs.tilt.toFixed(2)}deg`;
        }

        const y = gs.lastPoint.clientY;
        let delta = 0;
        if (y < EDGE)
            delta = -Math.ceil((EDGE - y) / 5);
        else if (y > window.innerHeight - EDGE)
            delta = Math.ceil((y - (window.innerHeight - EDGE)) / 5);
        if (!delta)
            return;
        const before = window.scrollY;
        window.scrollBy(0, delta);
        if (window.scrollY !== before)
            updatePointerGesture(gs.lastPoint);
    };
    autoScrollFrame = requestAnimationFrame(tick);
}

function stopAutoScroll() {
    cancelAnimationFrame(autoScrollFrame);
    autoScrollFrame = 0;
}

// Dragging a widget out of the shelf: a stand-in tile follows the pointer until it is dropped
// on the board.
function beginExternalDrag(def, tier, point) {
    if (gestureState || !ctx)
        return;
    if (layout.widgets.length >= (ctx.maxCards || 60)) {
        announce(`The board is full (${ctx.maxCards || 60} tiles).`);
        return;
    }
    closeCardMenus(false);
    selectTile(null);

    const m = metrics();
    const size = defaultSizeFor(def, tier);
    const start = { instanceId: INCOMING_ID, col: 0, row: 0, w: size.w, h: size.h };
    const rect = rectFor(start, m);

    const section = document.createElement("div");
    section.className = "dash-card is-dragging is-incoming";
    const face = document.createElement("div");
    face.className = "dash-card-face dash-incoming-face";
    const glyph = document.createElement("span");
    glyph.className = "dash-incoming-glyph";
    glyph.append(icon(def.id, 26));
    const name = document.createElement("span");
    name.className = "dash-incoming-name";
    name.textContent = getWidgetShortTitle(def);
    face.append(glyph, name);
    section.append(face);
    section.style.width = `${rect.w}px`;
    section.style.height = `${rect.h}px`;
    gridEl.append(section);

    gestureState = {
        mode: "add",
        instanceId: INCOMING_ID,
        bpName: currentBp.name,
        base: currentPlacements(),
        start,
        m,
        def,
        section,
        sizeBadge: null,
        startRect: rect,
        grabDX: rect.w / 2,
        grabDY: Math.min(rect.h / 2, 60),
        candidate: null,
        preview: currentPlacements(),
        lastPoint: point,
        lastX: point.clientX,
        tilt: 0,
    };
    rootEl.classList.add("is-gesturing", "shelf-peeking");
    updatePointerGesture(point);
    startAutoScroll();

    let rafPending = false;
    const onMove = e => {
        if (!gestureState || gestureState.mode !== "add")
            return;
        gestureState.lastPoint = { clientX: e.clientX, clientY: e.clientY };
        if (rafPending)
            return;
        rafPending = true;
        requestAnimationFrame(() => {
            rafPending = false;
            if (gestureState?.mode === "add")
                updatePointerGesture(gestureState.lastPoint);
        });
    };
    const cleanup = () => {
        document.removeEventListener("pointermove", onMove, true);
        document.removeEventListener("pointerup", onUp, true);
        document.removeEventListener("pointercancel", onCancel, true);
    };
    const onUp = e => {
        cleanup();
        if (gestureState?.mode === "add") {
            updatePointerGesture({ clientX: e.clientX, clientY: e.clientY });
            commitPointerGesture();
        }
    };
    const onCancel = () => {
        cleanup();
        if (gestureState?.mode === "add")
            cancelPointerGesture();
    };
    document.addEventListener("pointermove", onMove, true);
    document.addEventListener("pointerup", onUp, true);
    document.addEventListener("pointercancel", onCancel, true);
}

// --- Click a peg to move the selected tile there ------------------------------------------

function clearHoverCell() {
    if (!hoverCell)
        return;
    hoverCell = null;
    if (!gestureState) {
        dots.setFootprint(null);
        positionLanding(null, metrics());
    }
}

function placementAtPointer(e) {
    if (!isEditing || !selectedId || swapSourceId || gestureState || isCardMenuOpen())
        return null;
    if (e.target.closest?.(".dash-card"))
        return null;
    const tile = currentPlacements().find(p => p.instanceId === selectedId);
    if (!tile)
        return null;
    const m = metrics();
    const cell = cellFromPoint(e.clientX, e.clientY, m);
    return {
        col: Math.max(0, Math.min(cell.col, m.cols - tile.w)),
        row: Math.max(m.minRow, Math.min(cell.row, MAX_GRID_ROWS - tile.h)),
        w: tile.w,
        h: tile.h,
    };
}

boardEl.addEventListener("pointermove", e => {
    if (e.pointerType === "touch")
        return;
    const target = placementAtPointer(e);
    if (!target) {
        clearHoverCell();
        return;
    }
    if (hoverCell && hoverCell.col === target.col && hoverCell.row === target.row)
        return;
    hoverCell = target;
    dots.setFootprint(target);
    positionLanding(target, metrics());
});

boardEl.addEventListener("pointerleave", clearHoverCell);

boardEl.addEventListener("click", async e => {
    const target = placementAtPointer(e);
    if (!target)
        return;
    const id = selectedId;
    clearHoverCell();
    await commitTilePlacement(id, target, `Moved to column ${target.col + 1}, row ${target.row - metrics().minRow + 1}.`);
});

// Pressing and holding an empty part of the page starts editing, as it would on a phone's home screen.
let holdTimer = 0;
document.body.addEventListener("pointerdown", e => {
    if (isEditing || e.button !== 0 || !ctx)
        return;
    if (e.target.closest?.(".dash-card, .dash-card-menu, .dash-shelf, .dashboard-footer, .dashboard-banner, dialog, button, input, select, textarea, a"))
        return;
    const origin = { clientX: e.clientX, clientY: e.clientY };
    const cancelHold = () => {
        clearTimeout(holdTimer);
        document.removeEventListener("pointerup", cancelHold, true);
        document.removeEventListener("pointercancel", cancelHold, true);
        document.removeEventListener("pointermove", onHoldMove, true);
    };
    const onHoldMove = me => {
        if (Math.hypot(me.clientX - origin.clientX, me.clientY - origin.clientY) > 6)
            cancelHold();
    };
    clearTimeout(holdTimer);
    holdTimer = setTimeout(() => {
        cancelHold();
        setEditing(true, origin);
    }, 480);
    document.addEventListener("pointerup", cancelHold, true);
    document.addEventListener("pointercancel", cancelHold, true);
    document.addEventListener("pointermove", onHoldMove, true);
});

// --- Dialogs and the shelf --------------------------------------------------------------

async function openNewImageSettings(instanceId) {
    const entry = mountedCards.get(instanceId);
    if (!entry)
        return;
    picker.close();
    await entry.mountPromise;
    if (isEditing && !configDialog.open && !confirmDialog.open)
        openConfigureDialog(instanceId);
}

function openConfigureDialog(instanceId) {
    const entry = mountedCards.get(instanceId);
    if (!entry || typeof entry.controller?.renderConfig !== "function")
        return;
    const def = getWidgetDef(entry.instance.widgetType);
    configDialogTitle.textContent = `${def ? def.title : "Widget"} settings`;
    configDialogBody.replaceChildren();
    entry.controller.renderConfig(configDialogBody, () => hideDialog(configDialog));
    showDialog(configDialog);
}

function shelfState(returnFocusEl, extra) {
    return {
        ctx,
        placedWidgets: layout.widgets,
        cols: currentBp.cols,
        onSelectPreset: preset => openPresetConfirmation(preset),
        onDragOut: (def, tier, point) => beginExternalDrag(def, tier, point),
        onClose: () => {
            rootEl.classList.remove("shelf-open");
            updateToolbarState();
            returnFocusEl.focus({ preventScroll: true });
        },
        ...extra,
    };
}

function openAddPicker(initialTab = "widgets") {
    if (!ctx)
        return;
    if (!isEditing)
        setEditing(true);
    selectTile(null);
    picker.open(shelfState(initialTab === "presets" ? btnPresets : btnAddWidget, {
        mode: "add",
        initialTab,
        onSelectWidget: (def, _oldId, tier) => handleAddWidget(def, { tier }),
    }));
    rootEl.classList.add("shelf-open");
    updateToolbarState();
}

function openReplacePicker(instanceId) {
    if (!isEditing)
        setEditing(true);
    const inst = layout.widgets.find(w => w.instanceId === instanceId);
    const def = inst ? getWidgetDef(inst.widgetType) : null;
    picker.open(shelfState(mountedCards.get(instanceId)?.chrome.menuBtn || btnAddWidget, {
        mode: "replace",
        initialTab: "widgets",
        replaceInstanceId: instanceId,
        replaceTitle: def ? getWidgetShortTitle(def) : "tile",
        onSelectWidget: (newDef, oldId) => {
            picker.close();
            return handleReplaceWidget(newDef, oldId);
        },
    }));
    rootEl.classList.add("shelf-open");
    updateToolbarState();
}

function refreshShelf() {
    if (picker.isOpen())
        picker.update({ placedWidgets: layout.widgets, cols: currentBp.cols });
}

let confirmCallback = null;

function openConfirmDialog({ title, message, acceptLabel, extraEl = null, onConfirm }) {
    confirmTitle.textContent = title;
    confirmMsg.textContent = message;
    confirmAccept.textContent = acceptLabel;
    confirmExtra.replaceChildren();
    if (extraEl)
        confirmExtra.append(extraEl);
    confirmCallback = onConfirm;
    showDialog(confirmDialog);
}

confirmAccept.addEventListener("click", async () => {
    const cb = confirmCallback;
    confirmCallback = null;
    hideDialog(confirmDialog);
    if (cb)
        await cb();
});

function openPresetConfirmation(preset) {
    const extra = document.createElement("p");
    extra.className = "dialog-note";
    extra.textContent = preset.items.length === 0
        ? "Every tile comes off the board. Saved tasks, notes, reading queue and snippets are kept."
        : `It places ${preset.items.map(it => getWidgetDef(it.widgetType)?.title || it.widgetType).join(", ")}. Your saved tasks, notes and collections are kept.`;

    openConfirmDialog({
        title: `Use the ${preset.name} preset?`,
        message: "This replaces the current arrangement in one step. You can undo it afterwards.",
        acceptLabel: `Use ${preset.name}`,
        extraEl: extra,
        onConfirm: async () => {
            const nextWidgets = [];
            const wide = [];
            for (let i = 0; i < preset.items.length; ++i) {
                const it = preset.items[i];
                const def = getWidgetDef(it.widgetType);
                if (!def)
                    continue;
                const instanceId = `p-${preset.id}-${it.widgetType}-${i}-${Date.now().toString(36)}`;
                nextWidgets.push({
                    instanceId,
                    widgetType: it.widgetType,
                    configVersion: 1,
                    config: def.defaultConfig ? def.defaultConfig() : {},
                });
                wide.push({
                    instanceId,
                    col: it.col * GRID_STEPS_PER_CELL,
                    row: it.row * GRID_STEPS_PER_CELL,
                    w: it.w * GRID_STEPS_PER_CELL,
                    h: it.h * GRID_STEPS_PER_CELL,
                });
            }
            const nextPlacements = wide.length ? { wide: centerPlacements(wide) } : {};
            await commitLayoutChange(nextWidgets, nextPlacements, layout.compactMode, {
                announcement: `Applied the ${preset.name} preset.`,
            });
            refreshShelf();
        },
    });
}

// --- Dock -------------------------------------------------------------------------------

function startEditing(origin = null) {
    setEditing(true, origin);
    btnAddWidget.focus({ preventScroll: true });
}

btnCustomize.addEventListener("click", e => {
    const rect = btnCustomize.getBoundingClientRect();
    // A keyboard or scripted click has no position; the ripple then starts at the button.
    const origin = e.clientX || e.clientY
        ? { clientX: e.clientX, clientY: e.clientY }
        : { clientX: rect.left + rect.width / 2, clientY: rect.top + rect.height / 2 };
    startEditing(origin);
});

btnDoneEdit.addEventListener("click", () => {
    setEditing(false);
    (customizeHidden ? gridWrapEl : btnCustomize).focus({ preventScroll: true });
});

btnCustomizeHide.append(icon("close", 10));

// The host remembers the choice, for this browser session or in the board's own store, and
// tells the board's other tabs.
async function setCustomizeButton(visibility) {
    try {
        const res = await request("dashboard.setCustomizeButton", { visibility });
        customizeHidden = Boolean(res.hidden);
        updateToolbarState();
        if (customizeHidden) {
            const duration = visibility === "session" ? " until you quit the browser" : "";
            announce(`The Customize button is hidden${duration}. Right-click the dashboard background or focus the dashboard and press Enter for options.`);
            gridWrapEl.focus({ preventScroll: true });
        } else {
            announce("The Customize button is back.");
            (isEditing ? btnAddWidget : btnCustomize).focus({ preventScroll: true });
        }
    } catch (err) {
        setSaveStatus(err.message || "Couldn’t change the Customize button.", true);
    }
}

btnCustomizeHide.addEventListener("click", () => {
    openActionMenu(btnCustomizeHide, "Hide the Customize button", [
        { glyph: "close", label: "Cancel", onClick: () => {} },
        "separator",
        { glyph: "eye", label: "Hide for this session", keepsFocus: true, onClick: () => setCustomizeButton("session") },
        { glyph: "eye", label: "Hide forever", keepsFocus: true, onClick: () => setCustomizeButton("forever") },
    ]);
});

// Keep the board's controls reachable after the user dismisses the button.
function openDashboardOptions(point) {
    openActionMenu(gridWrapEl, "Dashboard options", [
        { glyph: "customize", label: "Customize dashboard", keepsFocus: true, onClick: () => startEditing() },
        { glyph: "eye", label: "Show Customize button", keepsFocus: true, onClick: () => setCustomizeButton("shown") },
    ], point);
}

gridWrapEl.addEventListener("contextmenu", e => {
    if (!customizeHidden || isEditing || e.target.closest(".dash-card, button, a, input, textarea, select"))
        return;
    e.preventDefault();
    openDashboardOptions({ x: e.clientX, y: e.clientY });
});

gridWrapEl.addEventListener("keydown", e => {
    if (!customizeHidden || isEditing || e.target !== gridWrapEl)
        return;
    if (e.key === "Enter" || e.key === "ContextMenu" || (e.shiftKey && e.key === "F10")) {
        e.preventDefault();
        openDashboardOptions({ x: 16, y: 16 });
    }
});

onDashboardEvent("customizeButtonChanged", data => {
    customizeHidden = Boolean(data?.hidden);
    updateToolbarState();
});

btnAddWidget.addEventListener("click", () => openAddPicker("widgets"));
btnPresets.addEventListener("click", () => openAddPicker("presets"));
emptyAddBtn.addEventListener("click", () => openAddPicker("widgets"));
emptyPresetBtn.addEventListener("click", () => openAddPicker("presets"));
swapCancelBtn.addEventListener("click", () => {
    clearSwapMode();
    announce("Cancelled the swap.");
});

btnCompact.addEventListener("click", async () => {
    const nextCompact = !layout.compactMode;
    const cur = currentPlacements();
    const resolved = nextCompact ? resolveLayout(cur, [], currentBp.cols, true, metrics(cur).minRow) : { ok: true, placements: cur };
    if (!resolved.ok)
        return;
    const nextPlacements = buildCommitPlacements(layout.widgets, currentBp.name, resolved.placements);
    await commitLayoutChange(layout.widgets, nextPlacements, nextCompact, {
        announcement: nextCompact ? "Tiles now stay packed toward the top." : "Tiles now stay where you put them.",
    });
});

btnUndo.addEventListener("click", async () => {
    if (!undoStack.length)
        return;
    const entry = undoStack.pop();
    const ok = await commitLayoutChange(
        entry.before.widgets,
        entry.before.placements,
        entry.before.compactMode,
        { recordUndo: false, announcement: "Undid the last change." }
    );
    // A failed commit has already refreshed the layout and cleared both stacks.
    if (ok)
        redoStack.push(entry);
    updateToolbarState();
});

btnRedo.addEventListener("click", async () => {
    if (!redoStack.length)
        return;
    const entry = redoStack.pop();
    const ok = await commitLayoutChange(
        entry.after.widgets,
        entry.after.placements,
        entry.after.compactMode,
        { recordUndo: false, announcement: "Redid the change." }
    );
    if (ok)
        undoStack.push(entry);
    updateToolbarState();
});

btnDeleteData.addEventListener("click", () => {
    openConfirmDialog({
        title: "Delete dashboard data?",
        message: "This removes your dashboard layout, notes, tasks, reading queue, habits, snippets, workspaces, and any opt-in feed or weather consent in this scope.",
        acceptLabel: "Delete dashboard data",
        onConfirm: async () => {
            try {
                const res = await request("dashboard.deleteAllData", {});
                layout = res.layout;
                customizeHidden = false;
                undoStack = [];
                redoStack = [];
                selectedId = null;
                for (const id of [...mountedCards.keys()])
                    unmountCard(id);
                renderGrid();
                updateToolbarState();
                refreshShelf();
                announce("Deleted dashboard data and restored the starter tiles.");
            } catch (err) {
                setSaveStatus(err.message || "Couldn’t delete dashboard data.", true);
            }
        },
    });
});

document.addEventListener("keydown", e => {
    if (e.key !== "Escape" || e.defaultPrevented)
        return;
    if (configDialog.open || confirmDialog.open)
        return;
    if (gestureState) {
        e.preventDefault();
        cancelPointerGesture();
    } else if (swapSourceId) {
        e.preventDefault();
        clearSwapMode();
        announce("Cancelled the swap.");
    } else if (picker.isOpen()) {
        e.preventDefault();
        picker.close();
    } else if (isEditing && selectedId) {
        e.preventDefault();
        selectTile(null);
    } else if (isEditing && !e.target.closest?.("input, textarea, select")) {
        e.preventDefault();
        setEditing(false);
        (customizeHidden ? gridWrapEl : btnCustomize).focus({ preventScroll: true });
    }
});

// The board follows its container. A breakpoint change re-projects the layout; neither writes anything.
let observedWidth = -1;
const resizeObserver = new ResizeObserver(entries => {
    const width = entries[0]?.contentRect?.width ?? gridWrapEl.clientWidth;
    // The board's own height changes as tiles move; only a change of width re-lays it out.
    if (Math.abs(width - observedWidth) < 0.5)
        return;
    observedWidth = width;
    const nextBp = getBreakpointForWidth(width);
    if (gestureState)
        cancelPointerGesture();
    if (nextBp.name !== currentBp.name) {
        closeCardMenus(false);
        currentBp = nextBp;
        refreshShelf();
    }
    if (ctx)
        withoutMotion(renderGrid);
});
resizeObserver.observe(gridWrapEl);

window.addEventListener("resize", () => {
    if (!ctx)
        return;
    if (gestureState)
        cancelPointerGesture();
    withoutMotion(renderGrid);
});

// Multi-tab live sync
onDashboardEvent("layoutUpdated", nextLayout => {
    if (!nextLayout)
        return;
    if (nextLayout.layoutRevision > layout.layoutRevision) {
        if (gestureState)
            cancelPointerGesture();
        layout = nextLayout;
        undoStack = [];
        redoStack = [];
        renderGrid();
        updateToolbarState();
        refreshShelf();
    }
});

onDashboardEvent("dataReset", freshLayout => {
    if (gestureState)
        cancelPointerGesture();
    layout = freshLayout;
    customizeHidden = false;
    undoStack = [];
    redoStack = [];
    selectedId = null;
    for (const id of [...mountedCards.keys()])
        unmountCard(id);
    renderGrid();
    updateToolbarState();
    refreshShelf();
});

// The host sends the browser's accent for the page's light and dark appearances, and
// dashboard.css derives every colored thing on the board from it.
const ACCENT_KEYS = { light: "light", lightFill: "light-fill", dark: "dark", darkFill: "dark-fill" };

function hexToTriplet(hex) {
    const m = /^#([0-9a-f]{2})([0-9a-f]{2})([0-9a-f]{2})$/i.exec(hex || "");
    return m ? `${parseInt(m[1], 16)}, ${parseInt(m[2], 16)}, ${parseInt(m[3], 16)}` : null;
}

function applyAccent(accent) {
    if (!accent || typeof accent !== "object")
        return;
    for (const [key, name] of Object.entries(ACCENT_KEYS)) {
        const value = accent[key];
        if (typeof value === "string" && /^#[0-9a-f]{6}$/i.test(value))
            rootEl.style.setProperty(`--page-accent-${name}`, value);
    }
    for (const key of ["light", "dark"]) {
        const triplet = hexToTriplet(accent[key]);
        if (triplet)
            rootEl.style.setProperty(`--page-accent-${key}-rgb`, triplet);
    }
    dots.refreshColors();
}

onDashboardEvent("accentChanged", applyAccent);

function showFallback() {
    fallbackEl.classList.remove("hidden");
    gridWrapEl.classList.add("hidden");
    footerEl.classList.add("hidden");
}

// Bootstrap
async function init() {
    const connected = await waitForBridge(2000);
    if (!connected) {
        showFallback();
        return;
    }

    try {
        const res = await request("dashboard.init", {});
        ctx = res.context;
        layout = res.layout;
        setExpectedGeneration(ctx.documentGeneration);

        if (ctx.isPrivate)
            rootEl.classList.add("is-private");
        applyAccent(ctx.accent);
        customizeHidden = Boolean(ctx.customizeHidden);
        dots.refreshColors();

        currentBp = getBreakpointForWidth(gridWrapEl.clientWidth || window.innerWidth);
        updateBanner();
        updateDefaultSaveStatus();
        withoutMotion(renderGrid);
        updateToolbarState();
        rootEl.classList.add("is-ready");
    } catch (err) {
        showFallback();
    }
}

init();
