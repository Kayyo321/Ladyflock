/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * A tile: the face a widget draws into, plus the handles that appear on it while the
 * board is being edited (pick-up surface, remove badge, options, resize corner).
 */

import { GRID_STEPS_PER_CELL, effectiveSizesForBreakpoint, formatGridSize } from "./grid-engine.js";
import { icon } from "./icons.js";
import { SIZE_NAMES, getWidgetShortTitle } from "./widget-registry.js";

const DRAG_THRESHOLD_PX = 4;

let activeMenuEl = null;
let activeMenuReturnFocus = null;

export function closeCardMenus(restoreFocus = false) {
    if (activeMenuEl) {
        activeMenuEl.remove();
        activeMenuEl = null;
    }
    if (activeMenuReturnFocus) {
        if (activeMenuReturnFocus.hasAttribute("aria-expanded"))
            activeMenuReturnFocus.setAttribute("aria-expanded", "false");
        if (restoreFocus && document.contains(activeMenuReturnFocus))
            activeMenuReturnFocus.focus();
    }
    activeMenuReturnFocus = null;
}

export function isCardMenuOpen() {
    return Boolean(activeMenuEl);
}

document.addEventListener("pointerdown", e => {
    if (activeMenuEl && !activeMenuEl.contains(e.target) && !e.target.closest?.(".dash-card-menu-btn, [aria-haspopup='menu']"))
        closeCardMenus(false);
});

document.addEventListener("keydown", e => {
    if (!activeMenuEl)
        return;
    if (e.key === "Escape") {
        e.preventDefault();
        e.stopPropagation();
        closeCardMenus(true);
        return;
    }
    if (e.key === "ArrowDown" || e.key === "ArrowUp" || e.key === "Home" || e.key === "End") {
        const items = [...activeMenuEl.querySelectorAll("button:not(:disabled)")];
        if (!items.length)
            return;
        e.preventDefault();
        const index = items.indexOf(document.activeElement);
        let next = e.key === "ArrowDown" ? index + 1 : index - 1;
        if (e.key === "Home")
            next = 0;
        else if (e.key === "End")
            next = items.length - 1;
        items[(next + items.length) % items.length].focus();
    }
}, true);

export function computeCardTier(placement, def, cols) {
    if (!def)
        return "default";
    const eff = effectiveSizesForBreakpoint(def.sizes, cols);
    const match = eff.find(s => s.w === placement.w && s.h === placement.h);
    if (match)
        return match.tier;
    if (placement.w <= 2 * GRID_STEPS_PER_CELL || placement.h <= GRID_STEPS_PER_CELL)
        return "compact";
    // A wide tile that is only two rows tall has no room for a large face's extra rows.
    if ((placement.w >= 6 * GRID_STEPS_PER_CELL && placement.h >= 3 * GRID_STEPS_PER_CELL) || placement.h >= 4 * GRID_STEPS_PER_CELL)
        return "expanded";
    return "default";
}

/** A small picture of a tile's footprint, used wherever a size is offered. */
export function createSizePictogram(w, h, cols = 12 * GRID_STEPS_PER_CELL) {
    const wrap = document.createElement("span");
    wrap.className = "size-pictogram";
    wrap.setAttribute("aria-hidden", "true");
    const shape = document.createElement("span");
    shape.className = "size-pictogram-shape";
    // 22 px is the tallest/widest the picture gets; keep the footprint's proportions.
    const unit = Math.min(22 / Math.max(w, 1), 22 / Math.max(h, 1), 5.5 / GRID_STEPS_PER_CELL);
    shape.style.width = `${Math.max(6, Math.round(Math.min(w, cols) * unit))}px`;
    shape.style.height = `${Math.max(6, Math.round(h * unit))}px`;
    wrap.append(shape);
    return wrap;
}

export function createCardElement({ instance, def, handlers }) {
    const id = instance.instanceId;
    const fullTitle = def ? def.title : instance.widgetType;

    const section = document.createElement("section");
    section.className = "dash-card";
    section.setAttribute("role", "listitem");
    section.dataset.instanceId = id;
    section.dataset.widgetType = instance.widgetType;
    section.setAttribute("aria-label", fullTitle);

    const face = document.createElement("div");
    face.className = "dash-card-face";

    const head = document.createElement("div");
    head.className = "dash-card-head";

    const glyph = document.createElement("span");
    glyph.className = "dash-card-glyph";
    glyph.append(icon(instance.widgetType, 15));

    const titleEl = document.createElement("h2");
    titleEl.className = "dash-card-title";
    titleEl.textContent = getWidgetShortTitle(def) || instance.widgetType;
    titleEl.title = fullTitle;

    const metaEl = document.createElement("span");
    metaEl.className = "dash-card-meta";

    head.append(glyph, titleEl, metaEl);

    const body = document.createElement("div");
    body.className = "dash-card-body";

    if (instance.widgetType !== "image")
        face.append(head);
    face.append(body);

    // The pick-up surface covers the tile while editing. It is the one focusable handle
    // for moving and resizing from the keyboard.
    const dragBtn = document.createElement("button");
    dragBtn.type = "button";
    dragBtn.className = "dash-card-drag";
    dragBtn.setAttribute(
        "aria-label",
        `${fullTitle}. Drag to move. Arrow keys move, Shift plus arrow keys resize, Enter opens options, Delete removes.`
    );

    let suppressClick = false;
    dragBtn.addEventListener("click", e => {
        e.preventDefault();
        if (suppressClick) {
            suppressClick = false;
            return;
        }
        handlers.onActivate(id);
    });
    dragBtn.addEventListener("focus", () => handlers.onFocusHandle(id));
    dragBtn.addEventListener("contextmenu", e => {
        e.preventDefault();
        openCardMenu(menuBtn, instance, def, handlers, { x: e.clientX, y: e.clientY });
    });
    dragBtn.addEventListener("keydown", e => {
        if (!handlers.isEditing())
            return;
        if (["ArrowLeft", "ArrowRight", "ArrowUp", "ArrowDown"].includes(e.key)) {
            e.preventDefault();
            const isRtl = getComputedStyle(section).direction === "rtl";
            let dx = 0;
            let dy = 0;
            if (e.key === "ArrowLeft")
                dx = isRtl ? 1 : -1;
            else if (e.key === "ArrowRight")
                dx = isRtl ? -1 : 1;
            else if (e.key === "ArrowUp")
                dy = -1;
            else
                dy = 1;
            if (e.shiftKey)
                handlers.onKeyboardResize(id, dx, dy);
            else
                handlers.onKeyboardMove(id, dx, dy);
        } else if (e.key === "Delete" || e.key === "Backspace") {
            e.preventDefault();
            handlers.onRemove(id);
        } else if (e.key === "Enter" || e.key === " ") {
            e.preventDefault();
            if (handlers.isSwapMode())
                handlers.onActivate(id);
            else
                openCardMenu(menuBtn, instance, def, handlers);
        }
    });
    attachDragGesture(dragBtn, id, "move", handlers, () => {
        suppressClick = true;
    });

    const menuBtn = document.createElement("button");
    menuBtn.type = "button";
    menuBtn.className = "dash-card-menu-btn";
    menuBtn.append(icon("more", 16));
    menuBtn.setAttribute("aria-haspopup", "menu");
    menuBtn.setAttribute("aria-expanded", "false");
    menuBtn.setAttribute("aria-label", `${fullTitle} options`);
    menuBtn.addEventListener("click", e => {
        e.stopPropagation();
        if (!handlers.isEditing())
            return;
        if (activeMenuEl && activeMenuReturnFocus === menuBtn) {
            closeCardMenus(false);
            return;
        }
        openCardMenu(menuBtn, instance, def, handlers);
    });

    const removeBtn = document.createElement("button");
    removeBtn.type = "button";
    removeBtn.className = "dash-card-remove";
    removeBtn.append(icon("minus", 14));
    removeBtn.setAttribute("aria-label", `Remove ${fullTitle}`);
    removeBtn.addEventListener("click", e => {
        e.stopPropagation();
        handlers.onRemove(id);
    });

    const resizeBtn = document.createElement("button");
    resizeBtn.type = "button";
    resizeBtn.className = "dash-card-resize";
    resizeBtn.tabIndex = -1;
    resizeBtn.setAttribute("aria-label", `Resize ${fullTitle}`);
    attachDragGesture(resizeBtn, id, "resize", handlers, () => {});

    const sizeBadge = document.createElement("span");
    sizeBadge.className = "dash-card-size-badge hidden";
    sizeBadge.setAttribute("aria-hidden", "true");

    section.append(face, dragBtn, menuBtn, removeBtn, resizeBtn, sizeBadge);
    return { section, face, head, body, metaEl, titleEl, dragBtn, menuBtn, removeBtn, resizeBtn, sizeBadge };
}

/**
 * A menu of plain actions for a control outside the tiles. `items` are
 * { glyph, label, onClick, danger } objects, or "separator".
 */
export function openActionMenu(anchor, label, items, point = null) {
    closeCardMenus(false);
    const menu = document.createElement("div");
    menu.className = "dash-card-menu";
    menu.setAttribute("role", "menu");
    menu.setAttribute("aria-label", label);
    for (const item of items) {
        if (item === "separator") {
            const sep = document.createElement("div");
            sep.className = "dash-menu-sep";
            sep.setAttribute("role", "separator");
            menu.append(sep);
            continue;
        }
        const btn = document.createElement("button");
        btn.type = "button";
        btn.className = "dash-menu-item" + (item.danger ? " danger" : "");
        btn.setAttribute("role", "menuitem");
        const text = document.createElement("span");
        text.textContent = item.label;
        btn.append(icon(item.glyph, 16), text);
        btn.addEventListener("click", () => {
            closeCardMenus(!item.keepsFocus);
            item.onClick();
        });
        menu.append(btn);
    }
    presentMenu(menu, anchor, point);
}

/** Records where a tile sits on the grid. Pixel geometry is applied by the board. */
export function applyPlacementData(section, placement, def, cols) {
    section.dataset.col = String(placement.col);
    section.dataset.row = String(placement.row);
    section.dataset.w = String(placement.w);
    section.dataset.h = String(placement.h);
    section.dataset.compactRow = placement.h <= GRID_STEPS_PER_CELL ? "true" : "false";
    section.dataset.narrow = placement.w <= 2 * GRID_STEPS_PER_CELL ? "true" : "false";
    section.dataset.tier = computeCardTier(placement, def, cols);
}

export function openCardMenu(anchorBtn, instance, def, handlers, point = null) {
    closeCardMenus(false);
    const id = instance.instanceId;
    const placement = handlers.getPlacement(id);
    const cols = handlers.cols();

    const menu = document.createElement("div");
    menu.className = "dash-card-menu";
    menu.setAttribute("role", "menu");
    menu.setAttribute("aria-label", `${def ? def.title : instance.widgetType} options`);

    const addItem = (glyphName, label, onClick, opts = {}) => {
        const btn = document.createElement("button");
        btn.type = "button";
        btn.className = "dash-menu-item" + (opts.danger ? " danger" : "");
        btn.setAttribute("role", "menuitem");
        btn.disabled = Boolean(opts.disabled);
        const text = document.createElement("span");
        text.textContent = label;
        btn.append(icon(glyphName, 16), text);
        btn.addEventListener("click", () => {
            closeCardMenus(false);
            onClick();
        });
        menu.append(btn);
        return btn;
    };

    const addSeparator = () => {
        const sep = document.createElement("div");
        sep.className = "dash-menu-sep";
        sep.setAttribute("role", "separator");
        menu.append(sep);
    };

    if (def && placement) {
        const validSizes = effectiveSizesForBreakpoint(def.sizes, cols);
        if (validSizes.length > 1) {
            const sizes = document.createElement("div");
            sizes.className = "dash-menu-sizes";
            sizes.setAttribute("role", "group");
            sizes.setAttribute("aria-label", "Size");
            for (const size of validSizes) {
                const isCurrent = size.w === placement.w && size.h === placement.h;
                const btn = document.createElement("button");
                btn.type = "button";
                btn.className = "dash-menu-size";
                btn.setAttribute("role", "menuitemradio");
                btn.setAttribute("aria-checked", isCurrent ? "true" : "false");
                const name = SIZE_NAMES[size.tier] || "Size";
                btn.setAttribute("aria-label", `${name}, ${formatGridSize(size, " by ")}`);
                const label = document.createElement("span");
                label.className = "dash-menu-size-label";
                label.textContent = name;
                btn.append(createSizePictogram(size.w, size.h, cols), label);
                btn.addEventListener("click", () => {
                    closeCardMenus(false);
                    if (!isCurrent)
                        handlers.onResizeTo(id, size.w, size.h);
                });
                sizes.append(btn);
            }
            menu.append(sizes);
            addSeparator();
        }
    }

    if (handlers.hasConfigure(id))
        addItem("configure", "Settings…", () => handlers.onOpenConfigure(id));
    addItem("swap", "Swap with…", () => handlers.onStartSwap(id), { disabled: handlers.cardCount() < 2 });
    addItem("replace", "Replace…", () => handlers.onOpenReplacePicker(id));
    if (handlers.isCustomizeHidden?.()) {
        addSeparator();
        addItem("eye", "Show Customize button", () => handlers.onShowCustomizeButton());
    }
    addSeparator();
    addItem("trash", "Remove", () => handlers.onRemove(id), { danger: true });

    presentMenu(menu, anchorBtn, point);
}

// Places a menu beside its anchor, or at a point, inside the window, and moves focus into it.
function presentMenu(menu, anchorBtn, point = null) {
    document.body.append(menu);
    const anchorRect = anchorBtn.getBoundingClientRect();
    const menuRect = menu.getBoundingClientRect();
    let left = point ? point.x : anchorRect.right - menuRect.width;
    let top = point ? point.y : anchorRect.bottom + 6;
    left = Math.max(8, Math.min(left, window.innerWidth - menuRect.width - 8));
    if (top + menuRect.height > window.innerHeight - 8)
        top = Math.max(8, (point ? point.y : anchorRect.top - 6) - menuRect.height);
    menu.style.left = `${left}px`;
    menu.style.top = `${top}px`;

    activeMenuEl = menu;
    activeMenuReturnFocus = anchorBtn;
    if (anchorBtn.hasAttribute("aria-expanded"))
        anchorBtn.setAttribute("aria-expanded", "true");

    const first = menu.querySelector('[aria-checked="true"]') || menu.querySelector("button:not(:disabled)");
    if (first)
        first.focus();
}

/**
 * Turns `handleEl` into a drag source. The gesture starts after the pointer has travelled
 * DRAG_THRESHOLD_PX, reports at most once per animation frame, and cancels on Escape,
 * pointer cancellation or loss of capture.
 */
export function attachDragGesture(handleEl, instanceId, mode, handlers, onDragStarted) {
    handleEl.addEventListener("pointerdown", e => {
        if (!handlers.isEditing() || e.button !== 0 || handlers.isSwapMode())
            return;
        if (mode === "resize")
            e.preventDefault();
        e.stopPropagation();

        const startX = e.clientX;
        const startY = e.clientY;
        let started = false;
        let rafPending = false;
        let lastX = startX;
        let lastY = startY;

        try {
            handleEl.setPointerCapture(e.pointerId);
        } catch {
            // The pointer is already gone; the listeners below never fire.
        }

        const cleanup = () => {
            handleEl.removeEventListener("pointermove", onMove);
            handleEl.removeEventListener("pointerup", onUp);
            handleEl.removeEventListener("pointercancel", cancel);
            handleEl.removeEventListener("lostpointercapture", cancel);
            window.removeEventListener("keydown", onKeyDown, true);
        };

        const cancel = () => {
            cleanup();
            if (started)
                handlers.onCancelGesture();
        };

        const onKeyDown = ke => {
            if (ke.key === "Escape") {
                ke.preventDefault();
                ke.stopPropagation();
                cancel();
            }
        };

        const onMove = me => {
            lastX = me.clientX;
            lastY = me.clientY;
            if (!started) {
                if (Math.hypot(lastX - startX, lastY - startY) < DRAG_THRESHOLD_PX)
                    return;
                started = true;
                onDragStarted();
                handlers.onBeginGesture(instanceId, mode, { clientX: startX, clientY: startY });
            }
            if (!rafPending) {
                rafPending = true;
                requestAnimationFrame(() => {
                    rafPending = false;
                    if (started)
                        handlers.onUpdateGesture(instanceId, mode, { clientX: lastX, clientY: lastY });
                });
            }
        };

        const onUp = ue => {
            cleanup();
            if (started) {
                started = false;
                handlers.onCommitGesture(instanceId, mode, { clientX: ue.clientX, clientY: ue.clientY });
            }
        };

        handleEl.addEventListener("pointermove", onMove);
        handleEl.addEventListener("pointerup", onUp);
        handleEl.addEventListener("pointercancel", cancel);
        handleEl.addEventListener("lostpointercapture", cancel);
        window.addEventListener("keydown", onKeyDown, true);
    });
}

export function renderCardError(bodyEl, message, onRetry, onRemove) {
    bodyEl.replaceChildren();
    const wrap = document.createElement("div");
    wrap.className = "dash-card-error";

    const msg = document.createElement("p");
    msg.textContent = message || "This widget couldn’t load.";

    const actions = document.createElement("div");
    actions.className = "dash-card-error-actions";

    const retryBtn = document.createElement("button");
    retryBtn.type = "button";
    retryBtn.className = "dash-btn";
    retryBtn.textContent = "Retry";
    retryBtn.addEventListener("click", onRetry);

    const removeBtn = document.createElement("button");
    removeBtn.type = "button";
    removeBtn.className = "dash-btn ghost";
    removeBtn.textContent = "Remove";
    removeBtn.addEventListener("click", onRemove);

    actions.append(retryBtn, removeBtn);
    wrap.append(msg, actions);
    bodyEl.append(wrap);
}
