/*
 * Copyright (c) 2026, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The other six Elements sidebar panes:
 * Computed (with Box Model & Rendered Fonts), Layout, Event Listeners,
 * DOM Breakpoints, Properties, and Accessibility.
 */

import { el, sendCommand, sendEngineCommand } from "./bridge.js";

const OVERLAY_PALETTE = ["#2c7be5", "#9b51e0", "#2f855a", "#d97706", "#e11d48", "#0891b2"];

export class SidebarsController {
    constructor(app) {
        this.app = app;
        this.computedFilter = "";
        this.computedShowAll = false;
        this.computedGrouped = false;
        this.computedData = {};
        this.layoutMetrics = {};
        this.usedFonts = [];
        this.gridLayouts = [];
        this.listeners = [];
        this.includeAncestorListeners = true;
        this.listenerFilter = "all";
        this.domBreakpoints = [];
        this.jsProperties = [];
        this.a11yTree = null;
        this.a11yDetails = null;
        this.fullA11yTreeMode = false;
    }

    // 1. Computed + Box Model + Rendered Fonts
    renderComputed() {
        const root = document.getElementById("sidebar-computed");
        root.textContent = "";

        // Interactive Box Model diagram
        root.appendChild(this.buildBoxModel());

        // Toolbar
        const filterInput = el("input", {
            type: "search",
            placeholder: "Filter computed properties",
            value: this.computedFilter,
            onInput: e => {
                this.computedFilter = e.target.value;
                this.renderComputedTable(tableHost);
            },
        });
        const showAllCb = el("input", {
            type: "checkbox",
            checked: this.computedShowAll,
            onChange: () => { this.computedShowAll = showAllCb.checked; this.renderComputedTable(tableHost); },
        });
        const groupedCb = el("input", {
            type: "checkbox",
            checked: this.computedGrouped,
            onChange: () => { this.computedGrouped = groupedCb.checked; this.renderComputedTable(tableHost); },
        });
        root.appendChild(el(
            "div",
            { class: "pane-toolbar" },
            filterInput,
            el("label", { class: "inline-check" }, showAllCb, "Show all"),
            el("label", { class: "inline-check" }, groupedCb, "Group")
        ));

        const tableHost = el("div");
        root.appendChild(tableHost);
        this.renderComputedTable(tableHost);

        // Rendered Fonts section
        const fontsSection = el("div", { class: "section-block" }, el("h3", { text: "Computed font list" }));
        if (this.usedFonts.length === 0) {
            fontsSection.appendChild(el("div", { class: "muted", text: "No computed font data for this node." }));
        } else {
            for (const f of this.usedFonts) {
                fontsSection.appendChild(el(
                    "div",
                    { style: "display:flex;justify-content:space-between;padding:3px 0" },
                    el("span", { text: `${f.family || f.name || "Unknown"} (${f.weight || 400}, ${f.size || ""})` }),
                    el("span", { class: "muted tabular", text: f.glyphCount !== undefined ? `${f.glyphCount} glyphs · ${f.source || "local"}` : (f.source || "computed") })
                ));
            }
        }
        root.appendChild(fontsSection);
    }

    buildBoxModel() {
        const m = this.layoutMetrics || {};
        const fmt = v => (v === undefined || v === null || v === "" ? "–" : String(v).replace(/px$/, ""));
        const editable = (prop, posClass, value) => el("button", {
            class: `inline-editor-trigger bm-val ${posClass}`,
            type: "button",
            disabled: value == null || !this.app.state.selectedNodeId || !!this.app.state.ghostSnapshot || !!this.app.state.selectedPseudo,
            text: fmt(value),
            title: `Edit ${prop}`,
            "aria-label": `Edit ${prop}, ${fmt(value)} pixels`,
            onClick: e => {
                e.stopPropagation();
                if (this.app.state.ghostSnapshot)
                    return;
                const next = prompt(`Set ${prop} (e.g. 12px):`, `${value}px`);
                if (next !== null) {
                    sendEngineCommand("editStyleDeclaration", {
                        nodeId: this.app.state.selectedNodeId,
                        isInline: true,
                        property: prop,
                        value: next.trim(),
                    }).then(() => this.app.refreshSelectedNodeProperties()).catch(err => this.app.showToast(err.message));
                }
            },
        });

        const w = m.width !== undefined ? Number(m.width).toFixed(2).replace(/\.00$/, "") : "–";
        const h = m.height !== undefined ? Number(m.height).toFixed(2).replace(/\.00$/, "") : "–";

        const content = el("div", { class: "bm-layer bm-content", text: `${w} × ${h}` });
        const padding = el(
            "div",
            { class: "bm-layer bm-padding" },
            el("span", { class: "bm-label", text: "padding" }),
            editable("padding-top", "top", m["padding-top"]),
            editable("padding-right", "right", m["padding-right"]),
            editable("padding-bottom", "bottom", m["padding-bottom"]),
            editable("padding-left", "left", m["padding-left"]),
            content
        );
        const border = el(
            "div",
            { class: "bm-layer bm-border" },
            el("span", { class: "bm-label", text: "border" }),
            editable("border-top-width", "top", m["border-top-width"]),
            editable("border-right-width", "right", m["border-right-width"]),
            editable("border-bottom-width", "bottom", m["border-bottom-width"]),
            editable("border-left-width", "left", m["border-left-width"]),
            padding
        );
        const margin = el(
            "div",
            { class: "bm-layer bm-margin" },
            el("span", { class: "bm-label", text: "margin" }),
            editable("margin-top", "top", m["margin-top"]),
            editable("margin-right", "right", m["margin-right"]),
            editable("margin-bottom", "bottom", m["margin-bottom"]),
            editable("margin-left", "left", m["margin-left"]),
            border
        );

        const wrap = el("div", { class: "box-model-wrap" }, margin);
        wrap.addEventListener("mouseenter", () => {
            if (this.app.state.selectedNodeId)
                sendCommand("highlightNode", { nodeId: this.app.state.selectedNodeId }).catch(() => {});
        });
        return wrap;
    }

    renderComputedTable(host) {
        host.textContent = "";
        const entries = Object.entries(this.computedData || {});
        const q = this.computedFilter.trim().toLowerCase();
        const filtered = entries.filter(([k, v]) => {
            if (!this.computedShowAll && (v === "none" || v === "normal" || v === "auto" || v === "0px" || v === "rgba(0, 0, 0, 0)"))
                return false;
            if (!q)
                return true;
            return k.toLowerCase().includes(q) || String(v).toLowerCase().includes(q);
        });

        const table = el("table", { class: "data-table monospace" });
        const tbody = el("tbody");
        for (const [prop, val] of filtered.sort((a, b) => a[0].localeCompare(b[0]))) {
            tbody.appendChild(el(
                "tr",
                {},
                el("td", { class: "decl-prop", text: prop }),
                el("td", { class: "decl-val", text: String(val) })
            ));
        }
        table.appendChild(tbody);
        host.appendChild(table);
    }

    // 2. Layout Pane (Concurrent Grid, Subgrid, Flex, Scroll-Snap, Container Query, Anchor)
    renderLayout() {
        const root = document.getElementById("sidebar-layout");
        root.textContent = "";

        const opts = this.app.state.gridOverlayOptions;
        const optSection = el("div", { class: "section-block" }, el("h3", { text: "Grid overlay display settings" }));
        const toggles = [
            ["showGridLineNumbers", "Show line numbers"],
            ["showGridAreasOverlay", "Show area names"],
            ["showTrackSizes", "Show track sizes"],
            ["showInfiniteLines", "Extend grid lines"],
        ];
        for (const [key, label] of toggles) {
            const cb = el("input", {
                type: "checkbox",
                checked: !!opts[key],
                onChange: () => {
                    opts[key] = cb.checked;
                    this.app.refreshActiveGridOverlays();
                },
            });
            optSection.appendChild(el("div", { style: "margin:4px 0" }, el("label", { class: "inline-check" }, cb, label)));
        }
        root.appendChild(optSection);

        // Grid & Flex nodes found in current DOM tree
        const gridNodes = [];
        const flexNodes = [];
        for (const node of this.app.elementsTree.nodesById.values()) {
            const d = String(node.display || "");
            if (d.includes("grid"))
                gridNodes.push(node);
            else if (d.includes("flex"))
                flexNodes.push(node);
        }

        const gridSec = el("div", { class: "section-block" }, el("h3", { text: `Grid overlays (${gridNodes.length})` }));
        if (gridNodes.length === 0)
            gridSec.appendChild(el("div", { class: "muted", text: "No grid containers in this document." }));
        gridNodes.forEach((node, idx) => {
            const active = this.app.state.activeGridOverlays.has(node.id);
            const color = this.app.state.overlayColors.get(node.id) || OVERLAY_PALETTE[idx % OVERLAY_PALETTE.length];
            const cb = el("input", {
                type: "checkbox",
                checked: active,
                onChange: () => this.app.toggleGridOverlay(node.id, color),
            });
            const colorInput = el("input", {
                type: "color",
                value: color,
                style: "width:22px;height:18px;padding:0;border:none;background:none",
                onInput: e => {
                    this.app.state.overlayColors.set(node.id, e.target.value);
                    if (this.app.state.activeGridOverlays.has(node.id))
                        this.app.refreshActiveGridOverlays();
                },
            });
            gridSec.appendChild(el(
                "div",
                { style: "display:flex;align-items:center;gap:8px;margin:4px 0" },
                cb,
                colorInput,
                el("button", {
                    class: "source-link monospace",
                    type: "button",
                    text: this.app.elementsTree.describeNodeShort(node),
                    onClick: () => this.app.elementsTree.selectNode(node.id, null, { reveal: true, focusRow: true }),
                })
            ));
        });
        root.appendChild(gridSec);

        const flexSec = el("div", { class: "section-block" }, el("h3", { text: `Flexbox overlays (${flexNodes.length})` }));
        if (flexNodes.length === 0)
            flexSec.appendChild(el("div", { class: "muted", text: "No flex containers in this document." }));
        flexNodes.forEach((node, idx) => {
            const active = this.app.state.activeFlexOverlays.has(node.id);
            const color = this.app.state.overlayColors.get(node.id) || OVERLAY_PALETTE[(idx + 2) % OVERLAY_PALETTE.length];
            const cb = el("input", {
                type: "checkbox",
                checked: active,
                onChange: () => this.app.toggleFlexOverlay(node.id, color),
            });
            flexSec.appendChild(el(
                "div",
                { style: "display:flex;align-items:center;gap:8px;margin:4px 0" },
                cb,
                el("button", {
                    class: "source-link monospace",
                    type: "button",
                    text: this.app.elementsTree.describeNodeShort(node),
                    onClick: () => this.app.elementsTree.selectNode(node.id, null, { reveal: true, focusRow: true }),
                })
            ));
        });
        root.appendChild(flexSec);

        // Loupe Latch & Extra Geometry Overlays
        const extraSec = el("div", { class: "section-block" }, el("h3", { text: "Loupe & Geometry Overlays" }));
        const latchCb = el("input", {
            type: "checkbox",
            checked: this.app.state.loupeLatched,
            onChange: () => {
                this.app.state.loupeLatched = latchCb.checked;
                this.app.showToast(latchCb.checked ? "Loupe latched to selected element" : "Loupe unlatched");
            },
        });
        extraSec.appendChild(el("div", { style: "margin:4px 0" }, el("label", { class: "inline-check" }, latchCb, "Latch Loupe overlay (Space)")));
        root.appendChild(extraSec);
    }

    // 3. Event Listeners
    async loadEventListeners() {
        const nodeId = this.app.state.selectedNodeId;
        if (!nodeId || this.app.state.ghostSnapshot) {
            this.listeners = [];
            this.renderEventListeners();
            return;
        }
        try {
            const res = await sendEngineCommand("getEventListeners", {
                nodeId,
                includeAncestors: this.includeAncestorListeners,
            });
            if (this.app.state.selectedNodeId !== nodeId || this.app.state.ghostSnapshot)
                return;
            this.listeners = res?.listeners || [];
        } catch {
            if (this.app.state.selectedNodeId !== nodeId)
                return;
            this.listeners = [];
        }
        this.renderEventListeners();
    }

    renderEventListeners() {
        const root = document.getElementById("sidebar-listeners");
        root.textContent = "";

        const ancCb = el("input", {
            type: "checkbox",
            checked: this.includeAncestorListeners,
            onChange: () => {
                this.includeAncestorListeners = ancCb.checked;
                this.loadEventListeners();
            },
        });
        const filterSel = el(
            "select",
            {
                onChange: e => { this.listenerFilter = e.target.value; this.renderEventListeners(); },
            },
            el("option", { value: "all", selected: this.listenerFilter === "all", text: "All listeners" }),
            el("option", { value: "blocking", selected: this.listenerFilter === "blocking", text: "Blocking only" }),
            el("option", { value: "passive", selected: this.listenerFilter === "passive", text: "Passive only" })
        );
        root.appendChild(el(
            "div",
            { class: "pane-toolbar" },
            el("label", { class: "inline-check" }, ancCb, "Ancestors"),
            filterSel,
            el("button", { class: "chip-btn", type: "button", text: "Refresh", onClick: () => this.loadEventListeners() })
        ));

        const filtered = this.listeners.filter(l => {
            if (this.listenerFilter === "passive")
                return l.passive;
            if (this.listenerFilter === "blocking")
                return !l.passive;
            return true;
        });

        if (filtered.length === 0) {
            root.appendChild(el("div", { class: "section-block muted", text: "No event listeners registered on this target." }));
            return;
        }

        // Group by event type
        const groups = new Map();
        for (const l of filtered) {
            if (!groups.has(l.type))
                groups.set(l.type, []);
            groups.get(l.type).push(l);
        }

        for (const [type, list] of groups.entries()) {
            const block = el("div", { class: "section-block" }, el("h3", { class: "monospace", text: `${type} (${list.length})` }));
            for (const item of list) {
                const row = el(
                    "div",
                    { style: "display:flex;align-items:center;justify-content:space-between;gap:8px;padding:3px 0" },
                    el(
                        "div",
                        {},
                        el("span", { class: "monospace", text: item.target || `node #${item.nodeId}` }),
                        el("span", { class: "monospace", text: item.handlerName }),
                        item.capture ? el("span", { class: "node-badge", text: "capture" }) : null,
                        item.passive ? el("span", { class: "node-badge", text: "passive" }) : null,
                        item.once ? el("span", { class: "node-badge", text: "once" }) : null,
                        item.extensionId ? el("span", { class: "node-badge ext", text: `ext:${item.extensionId}` }) : null
                    ),
                    el(
                        "div",
                        { style: "display:flex;gap:6px;align-items:center" },
                        item.source?.id
                            ? el("button", {
                                class: "source-link",
                                type: "button",
                                text: `${item.source.displayUrl || item.source.url || "Source"}:${item.line}`,
                                onClick: () => this.app.selectPanel("sources", item),
                            })
                            : null,
                        el("button", {
                            class: "chip-btn",
                            type: "button",
                            text: "Remove",
                            onClick: async () => {
                                await sendEngineCommand("removeEventListener", {
                                    nodeId: item.nodeId,
                                    listenerId: item.listenerId,
                                    windowTarget: item.windowTarget,
                                }).catch(err => this.app.showToast(err.message));
                                this.loadEventListeners();
                            },
                        })
                    )
                );
                block.appendChild(row);
            }
            root.appendChild(block);
        }
    }

    // 4. DOM Breakpoints
    setDomBreakpoints(list) {
        this.domBreakpoints = list || [];
        this.renderDomBreakpoints();
    }

    renderDomBreakpoints() {
        const root = document.getElementById("sidebar-breakpoints");
        root.textContent = "";

        if (this.domBreakpoints.length === 0) {
            root.appendChild(el("div", {
                class: "section-block muted",
                text: "No DOM breakpoints. Right-click a node in Elements to break on subtree, attribute, or removal mutations.",
            }));
            return;
        }

        for (const bp of this.domBreakpoints) {
            const cb = el("input", {
                type: "checkbox",
                checked: bp.enabled,
                onChange: () => sendCommand("setDomBreakpoint", {
                    nodeId: bp.nodeId,
                    type: bp.type,
                    nodePath: bp.nodePath,
                    enabled: cb.checked,
                }),
            });
            root.appendChild(el(
                "div",
                { class: "section-block", style: "display:flex;align-items:center;justify-content:space-between;gap:8px" },
                el(
                    "div",
                    { style: "display:flex;align-items:center;gap:8px" },
                    cb,
                    el(
                        "div",
                        {},
                        el("button", {
                            class: "source-link monospace",
                            type: "button",
                            text: bp.nodePath || `Node #${bp.nodeId}`,
                            onClick: () => bp.nodeId && this.app.elementsTree.selectNode(bp.nodeId, null, { reveal: true, focusRow: true }),
                        }),
                        el("div", { class: "muted", text: `${bp.type}${bp.resolved ? "" : " · unresolved after reload"}` })
                    )
                ),
                el("button", {
                    class: "chip-btn",
                    type: "button",
                    text: "Remove",
                    onClick: () => sendCommand("removeDomBreakpoint", { id: bp.id }),
                })
            ));
        }
    }

    // 5. Properties Pane (JS Wrapper + Prototype chain)
    async loadProperties() {
        const nodeId = this.app.state.selectedNodeId;
        const generation = this.app.state.documentGeneration;
        if (!nodeId || this.app.state.ghostSnapshot) {
            this.jsProperties = [];
            this.renderProperties();
            return;
        }
        try {
            const evalRes = await sendCommand("consoleEvaluate", { expression: "$0" });
            if (this.app.state.selectedNodeId !== nodeId || generation !== this.app.state.documentGeneration || this.app.state.ghostSnapshot)
                return;
            if (evalRes?.objectId) {
                const propsRes = await sendCommand("getObjectProperties", { objectId: evalRes.objectId, scope: "console" });
                if (this.app.state.selectedNodeId !== nodeId || generation !== this.app.state.documentGeneration || this.app.state.ghostSnapshot)
                    return;
                this.jsProperties = propsRes?.properties || [];
                this.jsPrototype = propsRes?.prototype || null;
            } else {
                this.jsProperties = [];
            }
        } catch {
            if (this.app.state.selectedNodeId !== nodeId || generation !== this.app.state.documentGeneration || this.app.state.ghostSnapshot)
                return;
            this.jsProperties = [];
        }
        this.renderProperties();
    }

    renderProperties() {
        const root = document.getElementById("sidebar-properties");
        root.textContent = "";

        root.appendChild(el(
            "div",
            { class: "pane-toolbar" },
            el("span", { class: "muted", text: "Own & inherited DOM wrapper properties ($0)" }),
            el("button", { class: "chip-btn", type: "button", text: "Refresh", onClick: () => this.loadProperties() })
        ));

        if (!this.jsProperties || this.jsProperties.length === 0) {
            root.appendChild(el("div", { class: "section-block muted", text: "Select an element to inspect its JS properties." }));
            return;
        }

        const table = el("table", { class: "data-table monospace" });
        const tbody = el("tbody");
        for (const p of this.jsProperties) {
            const valText = p.value
                ? (p.value.type === "string" ? JSON.stringify(p.value.value) : (p.value.text || String(p.value.value ?? p.value.className ?? p.value.type)))
                : (p.hasGetter ? "(...)" : "undefined");
            tbody.appendChild(el(
                "tr",
                { style: p.enumerable ? "" : "opacity:0.65" },
                el("td", { class: "decl-prop", text: p.name }),
                el("td", { class: "decl-val", text: valText })
            ));
        }
        table.appendChild(tbody);
        root.appendChild(table);
    }

    // 6. Accessibility Pane
    async loadAccessibility() {
        const generation = this.app.state.documentGeneration;
        const nodeId = this.app.state.selectedNodeId;
        this.a11yTree = null;
        this.a11yDetails = null;
        this.renderAccessibility();
        if (this.app.state.ghostSnapshot)
            return;
        const [tree, details] = await Promise.allSettled([
            sendCommand("getAccessibilityTree"),
            nodeId ? sendEngineCommand("getAccessibilityNodeDetails", { nodeId }) : null,
        ]);
        if (generation !== this.app.state.documentGeneration || nodeId !== this.app.state.selectedNodeId || this.app.state.ghostSnapshot)
            return;
        this.a11yTree = tree.value?.tree ?? null;
        this.a11yDetails = details.value ?? null;
        this.renderAccessibility();
    }

    findA11yNode(tree, domId) {
        if (!tree)
            return null;
        if (tree.id === domId)
            return tree;
        for (const c of tree.children || []) {
            const found = this.findA11yNode(c, domId);
            if (found)
                return found;
        }
        return null;
    }

    renderAccessibility() {
        const root = document.getElementById("sidebar-accessibility");
        root.textContent = "";

        const fullCb = el("input", {
            type: "checkbox",
            checked: this.fullA11yTreeMode,
            onChange: () => { this.fullA11yTreeMode = fullCb.checked; this.renderAccessibility(); },
        });
        root.appendChild(el(
            "div",
            { class: "pane-toolbar" },
            el("label", { class: "inline-check" }, fullCb, "Full tree"),
            el("button", { class: "chip-btn", type: "button", text: "Refresh", onClick: () => this.loadAccessibility() })
        ));

        const matched = this.findA11yNode(this.a11yTree, this.app.state.selectedNodeId);
        const info = this.a11yDetails || matched;

        const propsSec = el("div", { class: "section-block" }, el("h3", { text: "Computed Properties" }));
        if (!info) {
            propsSec.appendChild(el("div", { class: "muted", text: "Node is excluded from the accessibility tree or has no matching accessibility node." }));
        } else {
            const rows = [
                ["Role", info.role || "generic"],
                ["Name", info.name || "(empty)"],
                ["Description", info.description || "(none)"],
            ];
            if (typeof info.focusable === "boolean")
                rows.push(["Keyboard focusable", info.focusable ? "Yes" : "No"]);
            if (info.ignoredReason)
                rows.push(["Ignored reason", info.ignoredReason]);
            const t = el("table", { class: "data-table" });
            const tb = el("tbody");
            for (const [k, v] of rows)
                tb.appendChild(el("tr", {}, el("td", { class: "muted", text: k }), el("td", { class: "monospace", text: String(v) })));
            t.appendChild(tb);
            propsSec.appendChild(t);

            const nameInputs = info.nameInputs?.filter(input => input.present) || [];
            if (nameInputs.length) {
                propsSec.appendChild(el("h3", { text: "Name-related attributes and text" }));
                for (const input of nameInputs) {
                    propsSec.appendChild(el("div", {
                        class: "monospace",
                        style: "font-size:11px;padding:2px 0",
                        text: `${input.source}: ${JSON.stringify(input.value)}`,
                    }));
                }
            }
        }
        root.appendChild(propsSec);

        // Editable ARIA attributes from DOM node
        const domNode = this.app.state.selectedNode;
        const ariaAttrs = Object.entries(domNode?.attributes || {}).filter(([k]) => k === "role" || k.startsWith("aria-"));
        const ariaSec = el("div", { class: "section-block" }, el("h3", { text: "ARIA Attributes" }));
        if (ariaAttrs.length === 0) {
            ariaSec.appendChild(el("div", { class: "muted", text: "No ARIA attributes on this element." }));
        } else {
            for (const [k, v] of ariaAttrs) {
                const inp = el("input", {
                    type: "text",
                    value: String(v),
                    "aria-label": k,
                    disabled: !!this.app.state.ghostSnapshot || !!this.app.state.selectedPseudo,
                    style: "height:22px;font-size:11px",
                    onChange: () => sendCommand("setAttribute", {
                        nodeId: domNode.id,
                        name: k,
                        newName: k,
                        value: inp.value,
                        beforeValue: String(v),
                    }).then(() => this.loadAccessibility()).catch(err => this.app.showToast(err.message)),
                });
                ariaSec.appendChild(el("div", { style: "display:flex;align-items:center;gap:8px;margin:4px 0" }, el("span", { class: "monospace decl-prop", text: k }), inp));
            }
        }
        root.appendChild(ariaSec);

        if (this.fullA11yTreeMode && this.a11yTree) {
            const treeSec = el("div", { class: "section-block" }, el("h3", { text: "Accessibility Tree" }));
            const renderA11yBranch = (node, depth = 0) => {
                const canReveal = this.app.elementsTree.nodesById.has(node.id);
                const row = el(canReveal ? "button" : "div", {
                    class: `inline-editor-trigger dom-row monospace ${node.id === this.app.state.selectedNodeId ? "selected" : ""}`,
                    type: canReveal ? "button" : undefined,
                    style: `padding-left:${depth * 14 + 4}px`,
                    text: `${node.role || node.type || "node"}${node.name ? ` "${node.name}"` : ""}`,
                    onClick: canReveal ? () => this.app.elementsTree.selectNode(node.id, null, { reveal: true, focusRow: true }) : undefined,
                });
                treeSec.appendChild(row);
                for (const c of node.children || [])
                    renderA11yBranch(c, depth + 1);
            };
            renderA11yBranch(this.a11yTree, 0);
            root.appendChild(treeSec);
        }
    }
}
