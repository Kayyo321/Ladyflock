/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The widget shelf: a tray that rises from the bottom of the window while the board stays
 * in view above it. A widget is added with its plus button, or picked up and carried onto
 * the board. Everything shown here is bundled sample data; nothing subscribes or fetches.
 */

import { effectiveSizesForBreakpoint, formatGridSize } from "./grid-engine.js";
import { icon } from "./icons.js";
import { createSizePictogram } from "./widget-chrome.js";
import {
    CATALOG_SECTIONS,
    DASHBOARD_PRESETS,
    SIZE_NAMES,
    WIDGET_CATALOG,
    getWidgetDef,
    isWidgetAllowedInContext,
} from "./widget-registry.js";

const DRAG_OUT_THRESHOLD_PX = 6;

export function createPickerController({ shelfEl, titleEl, catsEl, tabWidgetsEl, tabPresetsEl, searchInputEl, bodyEl, closeBtn }) {
    let state = null;
    let category = "all"; // "all" | section id | "presets"
    const chosenTier = new Map(); // widget id -> tier picked on its card

    // Category chips sit between "All" and "Presets".
    const sectionChips = new Map();
    for (const section of CATALOG_SECTIONS) {
        const chip = document.createElement("button");
        chip.type = "button";
        chip.className = "shelf-cat";
        chip.setAttribute("role", "tab");
        chip.dataset.section = section.id;
        chip.textContent = section.short;
        chip.title = section.title;
        chip.addEventListener("click", () => setCategory(section.id));
        catsEl.insertBefore(chip, tabPresetsEl);
        sectionChips.set(section.id, chip);
    }

    function setCategory(next) {
        category = next;
        if (next !== "all")
            searchInputEl.value = "";
        render();
    }

    function isOpen() {
        return Boolean(state) && !shelfEl.classList.contains("hidden");
    }

    function renderChips(availableWidgets) {
        const select = (el, on) => {
            el.classList.toggle("active", on);
            el.setAttribute("aria-selected", on ? "true" : "false");
            el.tabIndex = on ? 0 : -1;
        };
        select(tabWidgetsEl, category === "all");
        select(tabPresetsEl, category === "presets");
        tabPresetsEl.classList.toggle("hidden", state.mode === "replace");
        for (const [id, chip] of sectionChips) {
            select(chip, category === id);
            chip.classList.toggle("hidden", !availableWidgets.some(w => w.section === id));
        }
        searchInputEl.closest(".picker-search").classList.toggle("is-muted", category === "presets");
    }

    function buildPresetCard(preset, eligibleItems) {
        const card = document.createElement("div");
        card.className = "picker-item picker-preset";

        const useBtn = document.createElement("button");
        useBtn.type = "button";
        useBtn.className = "dash-btn primary picker-use";
        useBtn.textContent = "Use";
        useBtn.setAttribute("aria-label", `Use the ${preset.name} preset`);
        useBtn.addEventListener("click", () => state.onSelectPreset({ ...preset, items: eligibleItems }));

        const rows = Math.max(3, ...eligibleItems.map(it => it.row + it.h));
        const map = document.createElement("div");
        map.className = "preset-map";
        map.setAttribute("aria-hidden", "true");
        map.style.aspectRatio = `12 / ${rows * 1.05}`;
        for (const it of eligibleItems) {
            const cell = document.createElement("span");
            cell.className = "preset-map-tile";
            cell.style.left = `${(it.col / 12) * 100}%`;
            cell.style.top = `${(it.row / rows) * 100}%`;
            cell.style.width = `${(it.w / 12) * 100}%`;
            cell.style.height = `${(it.h / rows) * 100}%`;
            map.append(cell);
        }

        const name = document.createElement("span");
        name.className = "picker-item-title";
        name.textContent = preset.name;

        const desc = document.createElement("p");
        desc.className = "picker-item-desc";
        desc.textContent = eligibleItems.length === 0
            ? "Clear the board. Your notes, tasks and collections stay saved."
            : eligibleItems.map(it => getWidgetDef(it.widgetType)?.title || it.widgetType).join(" · ");

        const head = document.createElement("div");
        head.className = "picker-preset-head";
        head.append(name, useBtn);

        card.append(map, head, desc);
        return card;
    }

    function buildWidgetCard(def, placedTypes) {
        const isReplace = state.mode === "replace";
        const isSingletonPlaced = !def.multi && placedTypes.has(def.id);
        const effSizes = effectiveSizesForBreakpoint(def.sizes, state.cols);
        let tier = chosenTier.get(def.id);
        if (!effSizes.some(s => s.tier === tier))
            tier = (effSizes[1] || effSizes[0]).tier;

        const card = document.createElement("div");
        card.className = "picker-item";
        card.dataset.widgetType = def.id;
        card.classList.toggle("is-placed", isSingletonPlaced);

        // First in the card: the button that puts it on the board.
        const addBtn = document.createElement("button");
        addBtn.type = "button";
        addBtn.className = "picker-add";
        if (isSingletonPlaced) {
            addBtn.disabled = true;
            addBtn.append(icon("check", 15));
            addBtn.setAttribute("aria-label", `${def.title} is already on the board`);
            addBtn.title = isReplace ? "Already on the board. Use Swap to trade places." : "Already on the board";
        } else {
            addBtn.append(icon(isReplace ? "replace" : "plus", 15));
            addBtn.setAttribute("aria-label", isReplace ? `Replace with ${def.title}` : `Add ${def.title}`);
            addBtn.addEventListener("click", () => state.onSelectWidget(def, state.replaceInstanceId, chosenTier.get(def.id) || null));
        }

        const art = document.createElement("div");
        art.className = "picker-art";
        art.append(icon(def.id, 18));

        const title = document.createElement("span");
        title.className = "picker-item-title";
        title.textContent = def.title;

        const desc = document.createElement("p");
        desc.className = "picker-item-desc";
        desc.textContent = def.description;

        const text = document.createElement("div");
        text.className = "picker-text";
        text.append(title, desc);

        const foot = document.createElement("div");
        foot.className = "picker-foot";

        if (!isReplace && effSizes.length > 1) {
            const sizes = document.createElement("div");
            sizes.className = "picker-sizes";
            sizes.setAttribute("role", "group");
            sizes.setAttribute("aria-label", `${def.title} size`);
            for (const size of effSizes) {
                const btn = document.createElement("button");
                btn.type = "button";
                btn.className = "picker-size";
                btn.disabled = isSingletonPlaced;
                btn.setAttribute("aria-pressed", size.tier === tier ? "true" : "false");
                btn.setAttribute("aria-label", `${SIZE_NAMES[size.tier] || "Size"}, ${formatGridSize(size, " by ")}`);
                btn.title = `${SIZE_NAMES[size.tier] || "Size"} · ${formatGridSize(size)}`;
                btn.append(createSizePictogram(size.w, size.h, state.cols));
                btn.addEventListener("click", () => {
                    chosenTier.set(def.id, size.tier);
                    for (const other of sizes.children)
                        other.setAttribute("aria-pressed", other === btn ? "true" : "false");
                });
                sizes.append(btn);
            }
            foot.append(sizes);
        }

        if (def.network) {
            const badge = document.createElement("span");
            badge.className = "dash-badge";
            badge.textContent = "Asks before going online";
            foot.append(badge);
        }

        card.append(addBtn, art, text, foot);

        // Pick the card up and carry it onto the board.
        if (!isSingletonPlaced && !isReplace) {
            card.classList.add("is-draggable");
            card.addEventListener("pointerdown", e => {
                if (e.button !== 0 || e.target.closest("button"))
                    return;
                const startX = e.clientX;
                const startY = e.clientY;
                const onMove = me => {
                    if (Math.hypot(me.clientX - startX, me.clientY - startY) < DRAG_OUT_THRESHOLD_PX)
                        return;
                    cleanup();
                    state.onDragOut(def, chosenTier.get(def.id) || null, { clientX: me.clientX, clientY: me.clientY });
                };
                const cleanup = () => {
                    document.removeEventListener("pointermove", onMove, true);
                    document.removeEventListener("pointerup", cleanup, true);
                    document.removeEventListener("pointercancel", cleanup, true);
                };
                document.addEventListener("pointermove", onMove, true);
                document.addEventListener("pointerup", cleanup, true);
                document.addEventListener("pointercancel", cleanup, true);
                e.preventDefault();
            });
        }

        return card;
    }

    function render() {
        if (!state)
            return;
        const { ctx, placedWidgets, mode } = state;
        const availableWidgets = WIDGET_CATALOG.filter(w => isWidgetAllowedInContext(w, ctx));
        const placedTypes = new Set(placedWidgets.map(w => w.widgetType));

        if (mode === "replace" && category === "presets")
            category = "all";
        renderChips(availableWidgets);

        titleEl.textContent = mode === "replace" ? `Replace ${state.replaceTitle || "tile"}` : "Widgets";
        shelfEl.setAttribute(
            "aria-description",
            mode === "replace"
                ? `Choose from ${availableWidgets.length} available widgets to replace this tile.`
                : `${availableWidgets.length} widgets available. Add one with its button, or drag it onto the board.`
        );

        const scrollTop = bodyEl.scrollTop;
        bodyEl.replaceChildren();

        if (category === "presets") {
            const grid = document.createElement("div");
            grid.className = "picker-grid picker-grid-presets";
            for (const preset of DASHBOARD_PRESETS) {
                const eligibleItems = preset.items.filter(item => isWidgetAllowedInContext(getWidgetDef(item.widgetType), ctx));
                if (preset.id !== "empty" && eligibleItems.length === 0)
                    continue;
                grid.append(buildPresetCard(preset, eligibleItems));
            }
            bodyEl.append(grid);
            return;
        }

        const q = searchInputEl.value.trim().toLowerCase();
        const matches = availableWidgets.filter(w => {
            if (category !== "all" && w.section !== category)
                return false;
            if (!q)
                return true;
            return w.title.toLowerCase().includes(q)
                || w.description.toLowerCase().includes(q)
                || w.id.toLowerCase().includes(q);
        });

        if (!matches.length) {
            const empty = document.createElement("p");
            empty.className = "shelf-empty";
            empty.textContent = "No widgets match that search.";
            bodyEl.append(empty);
            return;
        }

        const grid = document.createElement("div");
        grid.className = "picker-grid";
        for (const def of matches)
            grid.append(buildWidgetCard(def, placedTypes));
        bodyEl.append(grid);
        bodyEl.scrollTop = scrollTop;
    }

    tabWidgetsEl.addEventListener("click", () => setCategory("all"));
    tabPresetsEl.addEventListener("click", () => setCategory("presets"));
    catsEl.addEventListener("keydown", e => {
        if (!["ArrowLeft", "ArrowRight", "Home", "End"].includes(e.key))
            return;
        const tabs = [...catsEl.querySelectorAll('[role="tab"]:not(.hidden)')];
        const index = tabs.indexOf(e.target);
        if (index < 0)
            return;
        e.preventDefault();
        let next = e.key === "Home" ? 0 : tabs.length - 1;
        if (e.key === "ArrowLeft" || e.key === "ArrowRight") {
            const rtl = getComputedStyle(catsEl).direction === "rtl";
            const step = (e.key === "ArrowRight") !== rtl ? 1 : -1;
            next = (index + step + tabs.length) % tabs.length;
        }
        tabs[next].click();
        tabs[next].focus();
    });
    searchInputEl.addEventListener("input", () => {
        if (category !== "all")
            category = "all";
        render();
    });

    function close() {
        if (!state)
            return;
        const onClose = state.onClose;
        state = null;
        shelfEl.classList.remove("is-open");
        shelfEl.classList.add("hidden");
        onClose?.();
    }

    closeBtn.addEventListener("click", close);

    return {
        open(opts) {
            state = opts;
            category = opts.initialTab === "presets" && opts.mode !== "replace" ? "presets" : "all";
            searchInputEl.value = "";
            shelfEl.classList.remove("hidden");
            bodyEl.scrollTop = 0;
            render();
            // Rise on the next frame so the transition has a starting point.
            requestAnimationFrame(() => shelfEl.classList.add("is-open"));
            (category === "presets" ? tabPresetsEl : tabWidgetsEl).focus({ preventScroll: true });
        },
        update(partial) {
            if (!state)
                return;
            state = { ...state, ...partial };
            render();
        },
        close,
        isOpen,
    };
}
