/*
 * Copyright (c) 2026, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Elements DOM tree: single keyboard stop, ARIA tree semantics, inline editing,
 * F2 outerHTML editor, search (text/CSS/XPath), Ghost Snapshots, badges, and breadcrumbs.
 */

import { announce, el, sendCommand, sendEngineCommand } from "./bridge.js";

function nodeIdOf(node) {
    return node.id ?? node["parent-id"];
}

function pseudoOf(node) {
    return node.type === "pseudo-element" ? node.name : null;
}

// Elements that never have a closing tag in markup.
const VOID_ELEMENTS = new Set(["area", "base", "br", "col", "embed", "hr", "img", "input", "link", "meta", "source", "track", "wbr"]);

function isVoidElement(node) {
    return node.type === "element" && VOID_ELEMENTS.has(String(node.name || "").toLowerCase());
}

export class ElementsTreeController {
    constructor(app) {
        this.app = app;
        this.treeEl = document.getElementById("dom-tree");
        this.breadcrumbsEl = document.getElementById("dom-breadcrumbs");
        this.searchBarEl = document.getElementById("dom-search-bar");
        this.searchInputEl = document.getElementById("dom-search-input");
        this.searchCountEl = document.getElementById("dom-search-count");
        this.ghostBannerEl = document.getElementById("ghost-banner");

        this.rootNode = null;
        this.nodesById = new Map();
        this.parentById = new Map();
        this.expanded = new Set();
        this.selectedId = null;
        this.selectedPseudo = null;
        this.flatRows = [];
        this.searchMatches = [];
        this.searchIndex = -1;
        this.hiddenNodeIds = new Set();
        this.ghostSnapshot = null;
        this.hoverPreviewId = null;
        // While an inline editor is open, re-rendering would remove it; the render waits until the edit ends.
        this.activeEditors = 0;
        this.renderDeferred = false;

        this.bindEvents();
    }

    bindEvents() {
        this.treeEl.addEventListener("keydown", e => this.handleKeyDown(e));
        this.treeEl.addEventListener("mouseleave", () => {
            if (!this.app.state.loupeLatched)
                sendCommand("clearHighlight").catch(() => {});
        });

        document.getElementById("dom-search-prev").addEventListener("click", () => this.stepSearch(-1));
        document.getElementById("dom-search-next").addEventListener("click", () => this.stepSearch(1));
        document.getElementById("dom-search-close").addEventListener("click", () => this.closeSearch());
        this.searchInputEl.addEventListener("input", () => this.runSearch(this.searchInputEl.value));
        this.searchInputEl.addEventListener("keydown", e => {
            if (e.key === "Enter") {
                e.preventDefault();
                this.stepSearch(e.shiftKey ? -1 : 1);
            } else if (e.key === "Escape") {
                e.preventDefault();
                this.closeSearch();
            }
        });

        document.getElementById("btn-ghost-jump-parent").addEventListener("click", () => {
            this.jumpToSurvivingAncestor();
        });
    }

    resetDocument() {
        this.selectedId = null;
        this.selectedPseudo = null;
        this.lastKnownSelection = null;
        this.clearGhostSnapshot();
        this.expanded.clear();
        this.hiddenNodeIds.clear();
        this.flatRows = [];
        this.searchMatches = [];
        this.searchIndex = -1;
        this.searchInputEl.value = "";
        this.searchCountEl.textContent = "0 / 0";
        this.setTree(null);
        this.renderBreadcrumbs();
    }

    setTree(treeRoot) {
        this.rootNode = treeRoot;
        this.nodesById.clear();
        this.parentById.clear();
        this.indexTree(treeRoot, null);

        // Auto-expand root, html, and body on first load.
        if (this.expanded.size === 0 && treeRoot) {
            this.expanded.add(this.nodeKey(treeRoot));
            for (const child of treeRoot.children || []) {
                this.expanded.add(this.nodeKey(child));
                for (const grand of child.children || []) {
                    if (String(grand.name || "").toLowerCase() === "body")
                        this.expanded.add(this.nodeKey(grand));
                }
            }
        }

        // Check if currently selected node vanished -> preserve Ghost Snapshot!
        if (this.selectedId !== null && !this.nodesById.has(this.selectedId)) {
            this.activateGhostSnapshot();
        } else if (this.selectedId !== null) {
            this.clearGhostSnapshot();
            this.app.state.selectedNode = this.nodesById.get(this.selectedId);
            this.captureGhostSeed(this.selectedId);
            this.renderBreadcrumbs();
        }

        this.render();
    }

    indexTree(node, parentId) {
        if (!node)
            return;
        if (node.id !== undefined) {
            this.nodesById.set(node.id, node);
            if (parentId !== null)
                this.parentById.set(node.id, parentId);
        }
        for (const child of node.children || [])
            this.indexTree(child, node.id ?? parentId);
    }

    nodeKey(node) {
        if (node.type === "pseudo-element")
            return `pseudo:${node["parent-id"]}:${node["pseudo-element"]}`;
        return String(node.id);
    }

    selectNode(nodeId, pseudo = null, { reveal = true, focusRow = false } = {}) {
        if (!nodeId)
            return;
        this.selectedId = nodeId;
        this.selectedPseudo = pseudo;

        if (this.nodesById.has(nodeId)) {
            this.clearGhostSnapshot();
            if (reveal)
                this.expandAncestors(nodeId);
            this.captureGhostSeed(nodeId);
        } else {
            this.activateGhostSnapshot();
        }

        if (reveal) {
            this.render();
        } else {
            const previous = this.treeEl.querySelector(".dom-row.selected");
            previous?.classList.remove("selected");
            previous?.setAttribute("aria-selected", "false");
            previous?.removeAttribute("id");
            this.treeEl.removeAttribute("aria-activedescendant");
            const index = this.flatRows.findIndex(row => !row.isClosing && nodeIdOf(row.node) === nodeId && pseudoOf(row.node) === pseudo);
            const selected = this.treeEl.querySelector(`.dom-row[data-index="${index}"]`);
            if (selected) {
                selected.classList.add("selected");
                selected.setAttribute("aria-selected", "true");
                selected.id = "active-dom-row";
                this.treeEl.setAttribute("aria-activedescendant", selected.id);
            }
        }
        this.renderBreadcrumbs();
        this.app.onNodeSelected(nodeId, pseudo, this.nodesById.get(nodeId) || this.ghostSnapshot?.node || null);

        if (focusRow) {
            const row = this.treeEl.querySelector(".dom-row.selected");
            this.treeEl.focus({ preventScroll: true });
            row?.scrollIntoView({ block: "nearest" });
            // Moving through the tree from the keyboard shows the element on the page, as hovering its row does.
            if (!this.app.state.loupeLatched && this.nodesById.has(nodeId))
                sendCommand("highlightNode", { nodeId, pseudoElement: pseudo || "" }).catch(() => {});
        }
    }

    expandAncestors(nodeId) {
        let cur = this.parentById.get(nodeId);
        while (cur !== undefined) {
            this.expanded.add(String(cur));
            cur = this.parentById.get(cur);
        }
    }

    captureGhostSeed(nodeId) {
        const node = this.nodesById.get(nodeId);
        if (!node)
            return;
        const ancestors = [];
        let cur = this.parentById.get(nodeId);
        while (cur !== undefined) {
            ancestors.push(cur);
            cur = this.parentById.get(cur);
        }
        const previous = this.lastKnownSelection?.nodeId === nodeId && this.lastKnownSelection.pseudo === this.selectedPseudo
            ? this.lastKnownSelection : null;
        this.lastKnownSelection = {
            nodeId,
            pseudo: this.selectedPseudo,
            node: { ...node, children: undefined },
            ancestors,
            computed: previous?.computed ?? null,
            rules: previous?.rules ?? null,
        };
    }

    activateGhostSnapshot() {
        if (!this.lastKnownSelection || this.lastKnownSelection.nodeId !== this.selectedId || this.ghostSnapshot)
            return;
        this.ghostSnapshot = {
            ...this.lastKnownSelection,
            detachedAt: Date.now(),
        };
        const node = this.ghostSnapshot.node;
        const preview = document.getElementById("ghost-markup");
        let text = node.text ?? node.data ?? node.name ?? "";
        if (this.ghostSnapshot.pseudo) {
            text = this.ghostSnapshot.pseudo;
        } else if (node.type === "element") {
            const attrs = Object.entries(node.attributes || {}).map(([name, value]) =>
                ` ${name}="${String(value).replace(/&/g, "&amp;").replace(/"/g, "&quot;")}"`);
            text = `Captured opening tag\n<${String(node.name).toLowerCase()}${attrs.join("")}>`;
        }
        preview.textContent = text.length > 2048 ? text.slice(0, 2048) + "… (truncated)" : text;
        this.ghostBannerEl.classList.remove("hidden");
        announce("Selected element detached from document. Showing Ghost Snapshot.");
        this.app.onGhostSnapshotActive(this.ghostSnapshot);
    }

    clearGhostSnapshot() {
        if (!this.ghostSnapshot)
            return;
        this.ghostSnapshot = null;
        document.getElementById("ghost-capture").open = false;
        document.getElementById("ghost-markup").textContent = "";
        this.ghostBannerEl.classList.add("hidden");
        this.app.onGhostSnapshotActive(null);
    }

    jumpToSurvivingAncestor() {
        if (!this.ghostSnapshot)
            return;
        for (const ancestorId of this.ghostSnapshot.ancestors) {
            if (this.nodesById.has(ancestorId)) {
                this.selectNode(ancestorId, null, { reveal: true, focusRow: true });
                return;
            }
        }
        if (this.rootNode?.id !== undefined)
            this.selectNode(this.rootNode.id, null, { reveal: true, focusRow: true });
    }

    handleMutation(mutation) {
        if (!mutation || mutation.documentGeneration !== this.app.state.documentGeneration)
            return;
        // If our selected node was removed, trigger Ghost Snapshot immediately!
        if (mutation.removed && mutation.removed.includes(this.selectedId)) {
            this.nodesById.delete(this.selectedId);
            this.activateGhostSnapshot();
        }
        // Coalesced refresh from backend so tree stays exact.
        this.scheduleRefresh();
    }

    scheduleRefresh() {
        if (this.refreshTimer)
            return;
        this.refreshTimer = setTimeout(() => {
            this.refreshTimer = null;
            this.app.refreshDomTree();
        }, 60);
    }

    buildFlatRows(node, depth = 0, out = []) {
        if (!node)
            return out;
        if (node.type === "text" && !this.app.prefs.showWhitespace && !(node.text || "").trim())
            return out;
        if (!this.isShown(node))
            return out;

        const key = this.nodeKey(node);
        const children = (node.children || []).filter(
            c => this.isShown(c) && (c.type !== "text" || this.app.prefs.showWhitespace || (c.text || "").trim().length > 0)
        );
        const hasChildren = children.length > 0;
        const isExpanded = this.expanded.has(key);

        out.push({ node, key, depth, hasChildren, isExpanded, isClosing: false });

        if (hasChildren && isExpanded) {
            for (const child of children)
                this.buildFlatRows(child, depth + 1, out);
            if (node.type === "element" && !isVoidElement(node))
                out.push({ node, key: `${key}:close`, depth, hasChildren: false, isExpanded: false, isClosing: true });
        }
        return out;
    }

    beginEditing() {
        this.activeEditors++;
    }

    endEditing({ refocus }) {
        this.activeEditors = Math.max(0, this.activeEditors - 1);
        this.render();
        // Enter or Escape returns to the row; a click elsewhere keeps the focus it moved.
        if (refocus)
            this.treeEl.focus({ preventScroll: true });
    }

    // The picker hovers a node on the page: show its row without selecting it.
    previewNode(nodeId) {
        if (this.hoverPreviewId === nodeId)
            return;
        this.hoverPreviewId = nodeId;
        if (nodeId !== null && this.nodesById.has(nodeId))
            this.expandAncestors(nodeId);
        this.render();
        if (nodeId !== null)
            this.treeEl.querySelector(".dom-row.picker-hover")?.scrollIntoView({ block: "nearest" });
    }

    isShown(node) {
        return node.type !== "shadow-root" || !node.userAgent || this.app.prefs.showUserAgentShadowDom;
    }

    // Whether the node is in the tree as drawn, rather than inside a shadow root the tree hides.
    isNodeVisibleInTree(nodeId) {
        for (let cur = nodeId; cur !== undefined; cur = this.parentById.get(cur)) {
            const node = this.nodesById.get(cur);
            if (node && !this.isShown(node))
                return false;
        }
        return true;
    }

    render() {
        if (this.activeEditors > 0) {
            this.renderDeferred = true;
            return;
        }
        this.renderDeferred = false;
        // Rebuilding the rows must not move the tree: a page that keeps mutating re-renders it many times a second.
        const { scrollTop, scrollLeft } = this.treeEl;
        this.treeEl.removeAttribute("aria-activedescendant");
        this.treeEl.textContent = "";
        if (!this.rootNode) {
            this.treeEl.appendChild(el("div", { class: "dom-row muted", text: "Waiting for document…" }));
            return;
        }

        this.flatRows = this.buildFlatRows(this.rootNode, 0, []);
        const frag = document.createDocumentFragment();
        const matchSet = new Set(this.searchMatches);

        for (let i = 0; i < this.flatRows.length; i++) {
            const row = this.flatRows[i];
            const node = row.node;
            const isSelected = !row.isClosing && nodeIdOf(node) === this.selectedId && pseudoOf(node) === this.selectedPseudo;
            const rowEl = el("div", {
                class: [
                    "dom-row",
                    isSelected ? "selected" : "",
                    matchSet.has(node.id) && !row.isClosing ? "search-match" : "",
                    this.hiddenNodeIds.has(node.id) ? "hidden-node" : "",
                    !row.isClosing && this.hoverPreviewId !== null && nodeIdOf(node) === this.hoverPreviewId && !pseudoOf(node) ? "picker-hover" : "",
                ].filter(Boolean).join(" "),
                role: row.isClosing ? "presentation" : "treeitem",
                "aria-hidden": row.isClosing ? "true" : undefined,
                "aria-level": row.depth + 1,
                "aria-selected": isSelected ? "true" : "false",
                "aria-expanded": row.hasChildren ? (row.isExpanded ? "true" : "false") : undefined,
                dataset: { index: i, nodeId: node.id ?? "" },
            });
            if (isSelected) {
                rowEl.id = "active-dom-row";
                this.treeEl.setAttribute("aria-activedescendant", rowEl.id);
            }
            rowEl.style.paddingLeft = `${8 + row.depth * 14}px`;

            if (row.hasChildren) {
                const twisty = el("button", {
                    class: "dom-twisty",
                    type: "button",
                    tabindex: -1,
                    "aria-label": row.isExpanded ? "Collapse" : "Expand",
                    text: row.isExpanded ? "▾" : "▸",
                    onClick: e => {
                        e.stopPropagation();
                        this.toggleExpand(row.key, e.altKey);
                    },
                });
                rowEl.appendChild(twisty);
            } else {
                rowEl.appendChild(el("span", { class: "dom-twisty" }));
            }

            if (row.isClosing) {
                rowEl.appendChild(el("span", { class: "dom-tag", text: `</${String(node.name || "").toLowerCase()}>` }));
            } else {
                this.renderRowContent(rowEl, node, row.hasChildren && !row.isExpanded);
            }

            rowEl.addEventListener("click", e => {
                if (row.isClosing || e.target.closest("input, textarea, button"))
                    return;
                this.selectNode(nodeIdOf(node), pseudoOf(node), { reveal: false });
            });
            rowEl.addEventListener("dblclick", () => {
                if (!row.isClosing && node.type === "text")
                    this.beginInlineTextEdit(rowEl, node);
            });
            rowEl.addEventListener("mouseenter", () => {
                if (!this.app.state.loupeLatched && nodeIdOf(node))
                    sendCommand("highlightNode", { nodeId: nodeIdOf(node), pseudoElement: pseudoOf(node) || "" }).catch(() => {});
            });
            rowEl.addEventListener("contextmenu", e => {
                e.preventDefault();
                if (row.isClosing)
                    return;
                this.selectNode(nodeIdOf(node), pseudoOf(node), { reveal: false });
                this.showNodeContextMenu(e.clientX, e.clientY, node);
            });

            frag.appendChild(rowEl);
        }

        this.treeEl.appendChild(frag);
        this.treeEl.scrollTop = scrollTop;
        this.treeEl.scrollLeft = scrollLeft;
    }

    renderRowContent(rowEl, node, showEllipsis) {
        if (node.type === "document") {
            rowEl.appendChild(el("span", { class: "muted", text: "#document" }));
            if (node.url)
                rowEl.appendChild(el("span", { class: "muted", text: ` (${node.url})` }));
            return;
        }
        if (node.type === "shadow-root") {
            rowEl.appendChild(el("span", { class: "dom-pseudo", text: `#shadow-root (${node.userAgent ? "user-agent" : node.mode || "open"})` }));
            return;
        }
        if (node.type === "pseudo-element") {
            rowEl.appendChild(el("span", { class: "dom-pseudo", text: node.name || `::${node["pseudo-element"]}` }));
            return;
        }
        if (node.type === "comment") {
            rowEl.appendChild(el("span", { class: "dom-comment", text: `<!-- ${node.data || ""} -->` }));
            return;
        }
        if (node.type === "text") {
            const text = this.app.prefs.showWhitespace ? (node.text || "") : (node.text || "").replace(/\s+/g, " ").trim();
            rowEl.appendChild(el("span", { class: this.app.prefs.showWhitespace ? "dom-text preserve-whitespace" : "dom-text", text: `"${text}"` }));
            return;
        }

        if (node.type === "doctype") {
            rowEl.appendChild(el("span", { class: "muted", text: `<!DOCTYPE ${node.name}>` }));
            return;
        }
        if (node.type !== "element") {
            rowEl.appendChild(el("span", { class: "muted", text: node.name || "Node" }));
            return;
        }

        const markup = el("span");
        const tagName = String(node.name || "").toLowerCase();
        const tagSpan = el("span", { class: "dom-tag", text: `<${tagName}` });
        tagSpan.addEventListener("dblclick", e => {
            e.stopPropagation();
            this.beginInlineTagEdit(tagSpan, node);
        });
        markup.appendChild(tagSpan);

        if (node.attributes && typeof node.attributes === "object") {
            for (const [attrName, attrVal] of Object.entries(node.attributes)) {
                markup.appendChild(document.createTextNode(" "));
                const nameSpan = el("span", { class: "dom-attr-name", text: attrName });
                markup.appendChild(nameSpan);
                markup.appendChild(document.createTextNode('="'));
                const valSpan = el("span", { class: "dom-attr-val", text: String(attrVal) });
                valSpan.addEventListener("dblclick", e => {
                    e.stopPropagation();
                    this.beginInlineAttrEdit(valSpan, node, attrName, String(attrVal));
                });
                markup.appendChild(valSpan);
                markup.appendChild(document.createTextNode('"'));
            }
        }
        markup.appendChild(el("span", { class: "dom-tag", text: showEllipsis && !isVoidElement(node) ? ">…<" + `/${tagName}>` : ">" }));

        rowEl.appendChild(markup);

        // Badges with destinations (grid, flex, scroll, extension attribution, etc.)
        const display = String(node.display || "");
        if (display.includes("grid")) {
            const active = this.app.state.activeGridOverlays.has(node.id);
            rowEl.appendChild(el("button", {
                class: `node-badge ${active ? "active" : ""}`,
                type: "button",
                tabindex: -1,
                "aria-pressed": String(active),
                text: display.includes("subgrid") ? "subgrid" : "grid",
                title: "Toggle grid overlay",
                onClick: e => {
                    e.stopPropagation();
                    this.app.toggleGridOverlay(node.id);
                },
            }));
        } else if (display.includes("flex")) {
            const active = this.app.state.activeFlexOverlays.has(node.id);
            rowEl.appendChild(el("button", {
                class: `node-badge ${active ? "active" : ""}`,
                type: "button",
                tabindex: -1,
                "aria-pressed": String(active),
                text: "flex",
                title: "Toggle flex overlay",
                onClick: e => {
                    e.stopPropagation();
                    this.app.toggleFlexOverlay(node.id);
                },
            }));
        }
        if (node.scrollable) {
            rowEl.appendChild(el("span", { class: "node-badge", text: "scroll", title: "Scrollable container" }));
        }
        if (node.insertedBy) {
            rowEl.appendChild(el("span", {
                class: "node-badge ext",
                text: `ext:${node.insertedBy}`,
                title: `Inserted by ${node.insertedBy}`,
            }));
        }
    }

    renderBreadcrumbs() {
        this.breadcrumbsEl.textContent = "";
        if (!this.selectedId)
            return;
        const chain = [];
        let cur = this.selectedId;
        while (cur !== undefined && this.nodesById.has(cur)) {
            const n = this.nodesById.get(cur);
            if (n.type === "element" || n.type === "shadow-root")
                chain.unshift(n);
            cur = this.parentById.get(cur);
        }
        chain.forEach((n, idx) => {
            if (idx > 0)
                this.breadcrumbsEl.appendChild(el("span", { class: "muted", text: " › " }));
            const label = this.describeNodeShort(n);
            this.breadcrumbsEl.appendChild(el("button", {
                class: `crumb-btn ${n.id === this.selectedId ? "active" : ""}`,
                type: "button",
                text: label,
                "aria-current": n.id === this.selectedId ? "location" : undefined,
                onClick: () => this.selectNode(n.id, null, { reveal: true, focusRow: true }),
            }));
        });
        // The selected element is the last crumb; keep it in view when the path is wider than the bar.
        this.breadcrumbsEl.scrollLeft = this.breadcrumbsEl.scrollWidth;
    }

    describeNodeShort(node) {
        if (!node)
            return "";
        if (node.type === "shadow-root")
            return "#shadow-root";
        let label = String(node.name || "").toLowerCase();
        if (node.attributes?.id)
            label += `#${node.attributes.id}`;
        if (node.attributes?.class) {
            const classes = String(node.attributes.class).trim().split(/\s+/).filter(Boolean).slice(0, 2);
            if (classes.length)
                label += `.${classes.join(".")}`;
        }
        return label;
    }

    computeUniqueSelector(nodeId) {
        const parts = [];
        let cur = nodeId;
        while (cur !== undefined && this.nodesById.has(cur)) {
            const n = this.nodesById.get(cur);
            if (n.type !== "element")
                break;
            if (n.attributes?.id) {
                parts.unshift(`#${CSS.escape(String(n.attributes.id))}`);
                break;
            }
            let part = String(n.name || "").toLowerCase();
            if (n.attributes?.class) {
                const classes = String(n.attributes.class).trim().split(/\s+/).filter(Boolean);
                if (classes.length)
                    part += `.${classes.map(c => CSS.escape(c)).join(".")}`;
            }
            const parentId = this.parentById.get(cur);
            if (parentId !== undefined) {
                const parent = this.nodesById.get(parentId);
                const sameTagSiblings = (parent?.children || []).filter(c => c.type === "element" && c.name === n.name);
                if (sameTagSiblings.length > 1) {
                    const pos = sameTagSiblings.findIndex(c => c.id === cur) + 1;
                    part += `:nth-of-type(${pos})`;
                }
            }
            parts.unshift(part);
            cur = parentId;
        }
        return parts.join(" > ");
    }

    computeXPath(nodeId, full = false) {
        const parts = [];
        let cur = nodeId;
        while (cur !== undefined && this.nodesById.has(cur)) {
            const n = this.nodesById.get(cur);
            if (n.type !== "element")
                break;
            if (!full && n.attributes?.id) {
                parts.unshift(`//*[@id="${String(n.attributes.id).replace(/"/g, '\\"')}"]`);
                return parts.join("/");
            }
            const tag = String(n.name || "").toLowerCase();
            const parentId = this.parentById.get(cur);
            const parent = parentId !== undefined ? this.nodesById.get(parentId) : null;
            const sameTag = (parent?.children || []).filter(c => c.type === "element" && String(c.name || "").toLowerCase() === tag);
            const idx = sameTag.length > 1 ? `[${sameTag.findIndex(c => c.id === cur) + 1}]` : "";
            parts.unshift(`${tag}${idx}`);
            cur = parentId;
        }
        return `/${parts.join("/")}`;
    }

    toggleExpand(key, recursive = false) {
        if (this.expanded.has(key)) {
            this.expanded.delete(key);
        } else {
            this.expanded.add(key);
            if (recursive) {
                const node = this.nodesById.get(Number(key));
                const walk = n => {
                    if (!n)
                        return;
                    this.expanded.add(this.nodeKey(n));
                    for (const c of n.children || [])
                        walk(c);
                };
                walk(node);
            }
        }
        this.render();
    }

    handleKeyDown(e) {
        if (e.target.tagName === "INPUT" || e.target.tagName === "TEXTAREA")
            return;

        const selectable = this.flatRows.filter(r => !r.isClosing);
        const curIdx = selectable.findIndex(r => nodeIdOf(r.node) === this.selectedId && pseudoOf(r.node) === this.selectedPseudo);

        if (e.key === "ContextMenu" || (e.shiftKey && e.key === "F10")) {
            const row = this.treeEl.querySelector(".dom-row.selected");
            if (row && selectable[curIdx]) {
                e.preventDefault();
                const bounds = row.getBoundingClientRect();
                this.showNodeContextMenu(bounds.left, bounds.bottom, selectable[curIdx].node);
            }
        } else if (e.key === "ArrowDown") {
            e.preventDefault();
            const next = selectable[Math.min(selectable.length - 1, Math.max(0, curIdx + 1))];
            if (next)
                this.selectNode(nodeIdOf(next.node), pseudoOf(next.node), { reveal: false, focusRow: true });
        } else if (e.key === "ArrowUp") {
            e.preventDefault();
            const prev = selectable[Math.max(0, curIdx - 1)];
            if (prev)
                this.selectNode(nodeIdOf(prev.node), pseudoOf(prev.node), { reveal: false, focusRow: true });
        } else if (e.key === "ArrowRight") {
            e.preventDefault();
            const cur = selectable[curIdx];
            if (cur?.hasChildren) {
                if (!cur.isExpanded)
                    this.toggleExpand(cur.key, e.altKey);
                else if (selectable[curIdx + 1])
                    this.selectNode(nodeIdOf(selectable[curIdx + 1].node), pseudoOf(selectable[curIdx + 1].node), { reveal: false, focusRow: true });
            }
        } else if (e.key === "ArrowLeft") {
            e.preventDefault();
            const cur = selectable[curIdx];
            if (cur?.hasChildren && cur.isExpanded) {
                this.toggleExpand(cur.key);
            } else if (cur && pseudoOf(cur.node)) {
                this.selectNode(nodeIdOf(cur.node), null, { reveal: false, focusRow: true });
            } else if (cur && this.parentById.has(cur.node.id)) {
                this.selectNode(this.parentById.get(cur.node.id), null, { reveal: false, focusRow: true });
            }
        } else if (e.key === "F2") {
            e.preventDefault();
            if (!this.selectedPseudo && this.selectedId && this.nodesById.has(this.selectedId))
                this.openOuterHtmlEditor(this.nodesById.get(this.selectedId));
        } else if (e.key === "Delete" || e.key === "Backspace") {
            const node = this.nodesById.get(this.selectedId);
            if (!this.selectedPseudo && node && node.type === "element" && !this.ghostSnapshot) {
                e.preventDefault();
                sendCommand("removeNode", { nodeId: node.id, label: this.describeNodeShort(node) }).catch(() => {});
            }
        } else if (e.key.toLowerCase() === "h" && !e.metaKey && !e.ctrlKey && !e.altKey) {
            if (!this.selectedPseudo && this.selectedId && !this.ghostSnapshot) {
                e.preventDefault();
                this.toggleHideNode(this.selectedId);
            }
        }
    }

    beginInlineTagEdit(targetSpan, node) {
        const before = String(node.name || "").toLowerCase();
        const input = el("input", { class: "dom-inline-input", type: "text", value: before, "aria-label": "Tag name" });
        targetSpan.replaceWith(input);
        this.beginEditing();
        input.focus();
        input.select();
        let finished = false;
        const finish = (commit, refocus) => {
            if (finished)
                return;
            finished = true;
            const next = input.value.trim().toLowerCase();
            if (commit && next && next !== before) {
                sendCommand("setNodeTag", { nodeId: node.id, tag: next, beforeTag: before }).then(() => {
                    announce(`Changed tag to ${next}`);
                }).catch(err => this.app.showToast(err.message));
            }
            this.endEditing({ refocus });
        };
        input.addEventListener("keydown", e => {
            if (e.key === "Enter") { e.preventDefault(); finish(true, true); }
            else if (e.key === "Escape") { e.preventDefault(); finish(false, true); }
        });
        input.addEventListener("blur", () => finish(true, false), { once: true });
    }

    beginInlineAttrEdit(targetSpan, node, attrName, beforeVal) {
        const input = el("input", { class: "dom-inline-input", type: "text", value: beforeVal, "aria-label": `Value of ${attrName}` });
        targetSpan.replaceWith(input);
        this.beginEditing();
        input.focus();
        input.select();
        let finished = false;
        const finish = (commit, refocus) => {
            if (finished)
                return;
            finished = true;
            const next = input.value;
            if (commit && next !== beforeVal) {
                sendCommand("setAttribute", {
                    nodeId: node.id,
                    name: attrName,
                    newName: attrName,
                    value: next,
                    beforeValue: beforeVal,
                    label: this.describeNodeShort(node),
                }).then(() => {
                    // Shown as set until the tree, which refreshes after the page changes, replaces the node.
                    node.attributes = { ...(node.attributes || {}), [attrName]: next };
                    announce(`Updated ${attrName}`);
                }).catch(err => this.app.showToast(err.message));
            }
            this.endEditing({ refocus });
        };
        input.addEventListener("keydown", e => {
            if (e.key === "Enter") { e.preventDefault(); finish(true, true); }
            else if (e.key === "Escape") { e.preventDefault(); finish(false, true); }
        });
        input.addEventListener("blur", () => finish(true, false), { once: true });
    }

    beginInlineTextEdit(rowEl, node) {
        const before = node.text || "";
        const input = el("input", { class: "dom-inline-input", type: "text", value: before, "aria-label": "Text" });
        // Keep the twisty column so the text stays where it was drawn.
        rowEl.replaceChildren(el("span", { class: "dom-twisty" }), input);
        this.beginEditing();
        input.focus();
        input.select();
        let finished = false;
        const finish = (commit, refocus) => {
            if (finished)
                return;
            finished = true;
            if (commit && input.value !== before) {
                const text = input.value;
                sendCommand("setNodeText", { nodeId: node.id, text, beforeText: before })
                    .then(() => { node.text = text; })
                    .catch(err => this.app.showToast(err.message));
            }
            this.endEditing({ refocus });
        };
        input.addEventListener("keydown", e => {
            if (e.key === "Enter") { e.preventDefault(); finish(true, true); }
            else if (e.key === "Escape") { e.preventDefault(); finish(false, true); }
        });
        input.addEventListener("blur", () => finish(true, false), { once: true });
    }

    async openOuterHtmlEditor(node) {
        try {
            const res = await sendCommand("getNodeHtml", { nodeId: node.id, outer: true });
            const beforeHtml = res?.html || "";
            const row = this.treeEl.querySelector(`.dom-row[data-node-id="${node.id}"]`);
            if (!row)
                return;
            const ta = el("textarea", { class: "dom-html-editor", "aria-label": "Edit as HTML (Cmd/Ctrl+Enter to apply, Escape to cancel)" });
            ta.value = beforeHtml;
            row.insertAdjacentElement("afterend", ta);
            this.beginEditing();
            ta.focus();
            let finished = false;
            const finish = (apply, refocus) => {
                if (finished)
                    return;
                finished = true;
                const updated = ta.value;
                ta.remove();
                if (apply && updated !== beforeHtml) {
                    sendCommand("setNodeOuterHtml", {
                        nodeId: node.id,
                        html: updated,
                        beforeHtml,
                        label: this.describeNodeShort(node),
                    }).then(() => announce("Updated element HTML")).catch(err => this.app.showToast(err.message));
                }
                this.endEditing({ refocus });
            };
            ta.addEventListener("keydown", e => {
                if ((e.metaKey || e.ctrlKey) && e.key === "Enter") {
                    e.preventDefault();
                    finish(true, true);
                } else if (e.key === "Escape") {
                    e.preventDefault();
                    finish(false, true);
                }
            });
            ta.addEventListener("blur", () => finish(true, false), { once: true });
        } catch {
            // ignore
        }
    }

    async toggleHideNode(nodeId) {
        const hidden = !this.hiddenNodeIds.has(nodeId);
        try {
            await sendEngineCommand("setNodeHidden", { nodeId, hidden });
            if (hidden)
                this.hiddenNodeIds.add(nodeId);
            else
                this.hiddenNodeIds.delete(nodeId);
            this.render();
        } catch (err) {
            this.app.showToast(err.message);
        }
    }

    openSearch() {
        this.searchBarEl.classList.remove("hidden");
        this.searchInputEl.focus();
        this.searchInputEl.select();
    }

    closeSearch() {
        this.searchBarEl.classList.add("hidden");
        this.searchMatches = [];
        this.searchIndex = -1;
        this.searchCountEl.textContent = "0 / 0";
        this.render();
        this.treeEl.focus();
    }

    async runSearch(query) {
        const q = query.trim();
        if (!q) {
            this.searchMatches = [];
            this.searchIndex = -1;
            this.searchCountEl.textContent = "0 / 0";
            this.render();
            return;
        }

        // First try engine search (handles CSS selectors, XPath, shadow DOM, frames!).
        let ids = [];
        try {
            const res = await sendEngineCommand("searchDom", { query: q });
            ids = (res?.matches || []).map(match => match.nodeId);
        } catch {
            // Fallback local substring match across indexed DOM tree
            const lower = q.toLowerCase();
            for (const [id, node] of this.nodesById.entries()) {
                const hay = [
                    node.name || "",
                    node.text || "",
                    ...Object.entries(node.attributes || {}).flatMap(([k, v]) => [k, String(v)]),
                ].join(" ").toLowerCase();
                if (hay.includes(lower))
                    ids.push(id);
            }
        }

        if (q !== this.searchInputEl.value.trim())
            return;
        this.searchMatches = ids;
        this.searchIndex = ids.length > 0 ? 0 : -1;
        this.updateSearchDisplay();
    }

    stepSearch(delta) {
        if (this.searchMatches.length === 0)
            return;
        this.searchIndex = (this.searchIndex + delta + this.searchMatches.length) % this.searchMatches.length;
        this.updateSearchDisplay();
    }

    updateSearchDisplay() {
        const total = this.searchMatches.length;
        this.searchCountEl.textContent = total ? `${this.searchIndex + 1} / ${total}` : "0 / 0";
        announce(total ? `${total} matches` : "No matches");
        if (this.searchIndex >= 0) {
            this.selectNode(this.searchMatches[this.searchIndex], null, { reveal: true });
            this.treeEl.querySelector(".dom-row.selected")?.scrollIntoView({ block: "nearest" });
        } else {
            this.render();
        }
    }

    showNodeContextMenu(x, y, node) {
        const isElement = node.type === "element";
        const isDetached = !!this.ghostSnapshot;
        const selector = this.computeUniqueSelector(node.id);
        const items = [
            { label: "Add attribute", disabled: !isElement || isDetached, run: () => this.promptAddAttribute(node) },
            { label: "Edit as HTML (F2)", disabled: !isElement || isDetached, run: () => this.openOuterHtmlEditor(node) },
            { label: "Duplicate element", disabled: !isElement || isDetached, run: () => sendCommand("cloneNode", { nodeId: node.id }) },
            { label: "Delete element", disabled: !isElement || isDetached, run: () => sendCommand("removeNode", { nodeId: node.id, label: this.describeNodeShort(node) }) },
            { label: this.hiddenNodeIds.has(node.id) ? "Show element (H)" : "Hide element (H)", disabled: !isElement || isDetached, run: () => this.toggleHideNode(node.id) },
            { sep: true },
            { label: "Force state…", disabled: !isElement || isDetached, run: () => this.app.stylesPane.openForcedStateSubdrawer() },
            { label: "Break on subtree modifications", disabled: !isElement || isDetached, run: () => sendCommand("setDomBreakpoint", { nodeId: node.id, type: "subtree-modified", nodePath: selector }) },
            { label: "Break on attribute modifications", disabled: !isElement || isDetached, run: () => sendCommand("setDomBreakpoint", { nodeId: node.id, type: "attribute-modified", nodePath: selector }) },
            { label: "Break on node removal", disabled: !isElement || isDetached, run: () => sendCommand("setDomBreakpoint", { nodeId: node.id, type: "node-removed", nodePath: selector }) },
            { sep: true },
            { label: "Scroll into view", disabled: !isElement || isDetached, run: () => sendCommand("consoleEvaluate", { expression: "$0 && $0.scrollIntoView({ block: 'center' })" }) },
            { label: "Focus", disabled: !isElement || isDetached, run: () => sendCommand("consoleEvaluate", { expression: "$0 && $0.focus()" }) },
            { label: "Store as global variable", disabled: isDetached, run: () => this.storeAsGlobalVariable() },
            { label: "Capture node screenshot", disabled: !isElement || isDetached, run: () => this.captureNodeScreenshot(node) },
            { sep: true },
            // The markup of a detached node is no longer in the page to read.
            { label: "Copy outerHTML", disabled: !isElement || isDetached, run: () => this.copyHtml(node, true) },
            { label: "Copy selector", disabled: !isElement, run: () => sendCommand("copyText", { text: selector }) },
            { label: "Copy JS path", disabled: !isElement, run: () => sendCommand("copyText", { text: `document.querySelector(${JSON.stringify(selector)})` }) },
            { label: "Copy XPath", disabled: !isElement, run: () => sendCommand("copyText", { text: this.computeXPath(node.id, false) }) },
            { label: "Copy full XPath", disabled: !isElement, run: () => sendCommand("copyText", { text: this.computeXPath(node.id, true) }) },
            { sep: true },
            { label: "Create Cosmetic Filter…", disabled: !isElement || isDetached, run: () => this.app.stylesPane.openCosmeticFilterDrawer(selector) },
        ];
        if (isElement && node.display?.includes("grid"))
            items.unshift({ label: "Toggle grid overlay", run: () => this.app.toggleGridOverlay(node.id) });
        else if (isElement && node.display?.includes("flex"))
            items.unshift({ label: "Toggle flex overlay", run: () => this.app.toggleFlexOverlay(node.id) });
        this.app.showContextMenu(x, y, items);
    }

    async promptAddAttribute(node) {
        const row = this.treeEl.querySelector(`.dom-row[data-node-id="${node.id}"]`);
        if (!row)
            return;
        const input = el("input", { class: "dom-inline-input", type: "text", placeholder: 'name="value"', "aria-label": "New attribute" });
        row.appendChild(input);
        this.beginEditing();
        input.focus();
        let finished = false;
        const finish = (commit, refocus = false) => {
            if (finished)
                return;
            finished = true;
            const raw = input.value.trim();
            input.remove();
            this.endEditing({ refocus });
            if (!commit || !raw)
                return;
            const eq = raw.indexOf("=");
            const name = (eq >= 0 ? raw.slice(0, eq) : raw).trim();
            const val = eq >= 0 ? raw.slice(eq + 1).trim().replace(/^["']|["']$/g, "") : "";
            if (name) {
                sendCommand("setAttribute", {
                    nodeId: node.id,
                    name,
                    newName: name,
                    value: val,
                    isNew: true,
                    label: this.describeNodeShort(node),
                }).catch(err => this.app.showToast(err.message));
            }
        };
        input.addEventListener("keydown", e => {
            if (e.key === "Enter") { e.preventDefault(); finish(true, true); }
            else if (e.key === "Escape") { e.preventDefault(); finish(false, true); }
        });
        input.addEventListener("blur", () => finish(true, false), { once: true });
    }

    // As other browsers do: the next free tempN, so storing a second node keeps the first.
    async storeAsGlobalVariable() {
        try {
            const res = await sendCommand("consoleEvaluate", {
                expression: "(() => { let i = 1; while (`temp${i}` in globalThis) i++; globalThis[`temp${i}`] = $0; return `temp${i}`; })()",
            });
            if (res?.thrown || res?.type !== "string")
                throw new Error("Could not store the element.");
            this.app.showToast(`Stored as ${res.value}`);
        } catch (err) {
            this.app.showToast(err.message);
        }
    }

    async copyHtml(node, outer) {
        try {
            const res = await sendCommand("getNodeHtml", { nodeId: node.id, outer });
            if (res?.html !== undefined)
                await sendCommand("copyText", { text: res.html });
        } catch {
            // ignore
        }
    }

    async captureNodeScreenshot(node) {
        try {
            const res = await sendCommand("takeNodeScreenshot", { nodeId: node.id });
            if (res?.path)
                this.app.showToast(`Saved node screenshot to ${res.path}`);
        } catch (err) {
            this.app.showToast(err.message);
        }
    }
}
