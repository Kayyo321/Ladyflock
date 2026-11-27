/*
 * Copyright (c) 2026, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Ladyflock Inspector main controller: session binding, panels, command menu,
 * keyboard shortcuts, splitters, settings, and cross-panel reveals.
 */

import { announce, el, enableTabListKeyboard, enhanceScrollStrip, initBridge, onEvent, revealInStrip, sendCommand, sendEngineCommand } from "./bridge.js";
import { DrawersController } from "./drawers.js";
import { ElementsTreeController } from "./elements-tree.js";
import { SidebarsController } from "./sidebars.js";
import { StylesPaneController } from "./styles-pane.js";
import { ToolboxPanelsController } from "./toolbox-panels.js";

class InspectorApp {
    constructor() {
        this.state = {
            bound: false,
            attachmentId: 0,
            isPrivate: false,
            dockMode: "right",
            previousDockMode: "bottom",
            activePanel: "elements",
            activeSidebar: "styles",
            documentGeneration: 1,
            targetUrl: "",
            targetTitle: "",
            selectedNodeId: null,
            selectedPseudo: null,
            selectedNode: null,
            lastComputedStyle: null,
            lastAppliedRules: null,
            ghostSnapshot: null,
            pickerActive: false,
            loupeLatched: false,
            deviceModeActive: false,
            preserveConsoleLog: false,
            preserveNetworkLog: false,
            activeGridOverlays: new Set(),
            activeFlexOverlays: new Set(),
            overlayColors: new Map(),
            gridOverlayOptions: {
                showGridLineNumbers: true,
                showGridAreasOverlay: true,
                showTrackSizes: true,
                showInfiniteLines: false,
            },
        };

        this.prefs = {
            showWhitespace: false,
            showUserAgentShadowDom: false,
            useApcaContrast: false,
            showUaStyles: false,
        };

        this.elementsTree = new ElementsTreeController(this);
        this.stylesPane = new StylesPaneController(this);
        this.sidebars = new SidebarsController(this);
        this.drawers = new DrawersController(this);
        this.toolboxPanels = new ToolboxPanelsController(this);

        initBridge();
        this.bindEvents();
        this.bindProtocolEvents();
        this.setupSplitters();

        document.addEventListener("WebUILoaded", () => {
            sendCommand("getSessionState").catch(() => {});
        });
        if (window.ladybird)
            sendCommand("getSessionState").catch(() => {});
    }

    currentTargetHost() {
        try {
            return new URL(this.state.targetUrl).hostname;
        } catch {
            return "";
        }
    }

    bindEvents() {
        // Panel tabs
        document.querySelectorAll("#panel-tabs button").forEach(btn => {
            btn.addEventListener("click", () => this.selectPanel(btn.dataset.panel));
        });

        // Sidebar tabs
        document.querySelectorAll("#sidebar-tabs button").forEach(btn => {
            btn.addEventListener("click", () => this.selectSidebar(btn.dataset.sidebar));
        });
        for (const id of ["panel-tabs", "sidebar-tabs", "drawer-tabs", "dom-breadcrumbs"])
            enhanceScrollStrip(document.getElementById(id));
        enableTabListKeyboard(document.getElementById("panel-tabs"), tab => this.selectPanel(tab.dataset.panel));
        enableTabListKeyboard(document.getElementById("sidebar-tabs"), tab => this.selectSidebar(tab.dataset.sidebar));

        // Top toolbar actions
        document.getElementById("btn-pick").addEventListener("click", () => {
            sendCommand("setPickerActive", { active: !this.state.pickerActive }).catch(() => {});
        });
        document.getElementById("btn-device-mode").addEventListener("click", () => this.toggleDeviceMode());
        document.getElementById("btn-drawer-toggle").addEventListener("click", () => this.drawers.toggle());
        document.getElementById("btn-close").addEventListener("click", () => sendCommand("closeInspector").catch(() => {}));

        // Dock & More dropdowns
        this.setupDropdown("btn-dock", "dock-menu");
        this.setupDropdown("btn-more", "more-menu");

        document.querySelectorAll("#dock-menu button").forEach(btn => {
            btn.addEventListener("click", () => {
                this.closeAllDropdowns();
                this.setDockMode(btn.dataset.dock);
            });
        });
        document.querySelectorAll("#more-menu button").forEach(btn => {
            btn.addEventListener("click", () => {
                this.closeAllDropdowns();
                this.runNamedCommand(btn.dataset.cmd);
            });
        });

        // Device mode controls
        const presetSel = document.getElementById("device-preset");
        presetSel.addEventListener("change", () => {
            const m = presetSel.value.match(/^(\d+)x(\d+)@(\d+)$/);
            if (m) {
                document.getElementById("device-width").value = m[1];
                document.getElementById("device-height").value = m[2];
                document.getElementById("device-dpr").value = m[3];
                this.applyDeviceMode();
            }
        });
        document.getElementById("btn-device-rotate").addEventListener("click", () => {
            const w = document.getElementById("device-width");
            const h = document.getElementById("device-height");
            [w.value, h.value] = [h.value, w.value];
            this.applyDeviceMode();
        });
        document.getElementById("btn-device-apply").addEventListener("click", () => this.applyDeviceMode());
        document.getElementById("btn-device-reset").addEventListener("click", () => {
            this.toggleDeviceMode(false);
        });

        // Global keyboard shortcuts scoped to Inspector focus
        document.addEventListener("keydown", e => this.handleGlobalKeyDown(e));
        // The Inspector has its own menus. The page menu would offer Reload, Back and View Source for the Inspector
        // itself; text fields keep theirs for cut, copy and paste.
        document.addEventListener("contextmenu", e => {
            if (!e.target.closest("input, textarea, [contenteditable]"))
                e.preventDefault();
        });
        document.addEventListener("click", e => {
            if (!e.target.closest("#context-menu"))
                this.hideContextMenu();
            if (!e.target.closest(".menu-anchor"))
                this.closeAllDropdowns();
        });

        // Command menu & settings dialogs
        const commandInput = document.getElementById("command-menu-input");
        commandInput.addEventListener("input", e => this.renderCommandMenu(e.target.value));
        commandInput.addEventListener("keydown", e => {
            const items = [...document.querySelectorAll("#command-menu-list .command-item")];
            if (e.key === "ArrowDown" || e.key === "ArrowUp") {
                e.preventDefault();
                if (items.length)
                    this.setActiveCommand((this.commandMenuIndex + (e.key === "ArrowDown" ? 1 : -1) + items.length) % items.length);
            } else if (e.key === "Enter") {
                e.preventDefault();
                items[this.commandMenuIndex]?.click();
            }
        });
        for (const menuId of ["dock-menu", "more-menu", "context-menu"])
            this.enableMenuKeyboard(document.getElementById(menuId));
        window.addEventListener("resize", () => this.hideContextMenu());
        window.addEventListener("blur", () => this.hideContextMenu());
        // Scrolling elsewhere dismisses the menu. A wheel, not a scroll event: panes also scroll when their content
        // changes, as the Styles pane does when the menu's own right-click selects another element.
        document.addEventListener("wheel", e => {
            if (!e.target.closest?.("#context-menu"))
                this.hideContextMenu();
        }, { capture: true, passive: true });
        document.getElementById("btn-settings-close").addEventListener("click", () => document.getElementById("settings-dialog").close());
    }

    bindProtocolEvents() {
        onEvent("inspectorState", data => {
            this.state.bound = !!data.bound;
            const unboundEl = document.getElementById("unbound-state");
            const workspaceEl = document.getElementById("workspace");
            if (!this.state.bound) {
                unboundEl.classList.remove("hidden");
                workspaceEl.classList.add("hidden");
                if (data.reason)
                    document.getElementById("unbound-reason").textContent = data.reason;
                return;
            }
            unboundEl.classList.add("hidden");
            workspaceEl.classList.remove("hidden");

            this.state.attachmentId = data.attachmentId || 0;
            this.state.isPrivate = !!data.isPrivate;
            this.state.dockMode = data.dockMode || "right";
            this.state.documentGeneration = data.documentGeneration || 1;
            this.state.targetUrl = data.targetUrl || "";
            this.state.targetTitle = data.targetTitle || "";
            this.state.pickerActive = !!data.pickerActive;

            document.body.dataset.private = this.state.isPrivate ? "true" : "false";
            this.syncDockMenuUI();
            this.syncPickerButton();

            if (data.consoleEntries?.entries)
                this.drawers.consoleEntries = data.consoleEntries.entries;
            if (data.networkEntries?.entries)
                this.toolboxPanels.setNetworkEntries(data.networkEntries.entries);
            if (data.editJournal)
                this.drawers.setEditJournal(data.editJournal);
            if (data.domBreakpoints?.breakpoints)
                this.sidebars.setDomBreakpoints(data.domBreakpoints.breakpoints);

            if (data.initialPanel)
                this.selectPanel(data.initialPanel);

            this.refreshDomTree().then(() => {
                if (data.selectedNodeId && data.documentGeneration === this.state.documentGeneration
                    && this.elementsTree.nodesById.has(data.selectedNodeId))
                    this.elementsTree.selectNode(data.selectedNodeId, null, { reveal: true, focusRow: true });
            });
        });

        onEvent("domTree", data => {
            if (data?.tree)
                this.elementsTree.setTree(data.tree);
        });

        onEvent("domMutation", data => {
            this.elementsTree.handleMutation(data);
        });

        onEvent("domNodeProperties", data => {
            if (!data || this.state.ghostSnapshot || data.documentGeneration !== this.state.documentGeneration
                || data.nodeId !== this.state.selectedNodeId || (data.pseudoElement || null) !== this.state.selectedPseudo)
                return;
            const snapshot = this.elementsTree.lastKnownSelection;
            if (data.type === "appliedStyleRules") {
                if (snapshot)
                    snapshot.rules = data.properties;
                this.state.lastAppliedRules = data.properties;
                this.stylesPane.setRules(data.properties);
            } else if (data.type === "computedStyle") {
                if (snapshot)
                    snapshot.computed = data.properties;
                this.state.lastComputedStyle = data.properties || {};
                this.sidebars.computedData = this.state.lastComputedStyle;
                if (this.state.activeSidebar === "computed")
                    this.sidebars.renderComputed();
            } else if (data.type === "layout") {
                this.sidebars.layoutMetrics = data.properties || {};
                if (this.state.activeSidebar === "computed")
                    this.sidebars.renderComputed();
            } else if (data.type === "usedFonts") {
                this.sidebars.usedFonts = Array.isArray(data.properties) ? data.properties : (data.properties?.fonts || []);
                if (this.state.activeSidebar === "computed")
                    this.sidebars.renderComputed();
            }
        });

        onEvent("selectNode", data => {
            if (data?.documentGeneration !== undefined && data.documentGeneration !== this.state.documentGeneration)
                return;
            if (!data?.nodeId) {
                this.showBanner("Element is no longer available.");
                return;
            }
            this.clearBanner();
            this.selectPanel("elements");
            if (this.elementsTree.nodesById.has(data.nodeId)) {
                this.elementsTree.selectNode(data.nodeId, null, { reveal: true, focusRow: true });
            } else {
                const generation = this.state.documentGeneration;
                this.refreshDomTree().then(() => {
                    if (generation !== this.state.documentGeneration)
                        return;
                    if (this.elementsTree.nodesById.has(data.nodeId))
                        this.elementsTree.selectNode(data.nodeId, null, { reveal: true, focusRow: true });
                    else
                        this.showBanner("Element is no longer available.");
                });
            }
        });

        onEvent("selectPanel", data => {
            if (data?.panel)
                this.selectPanel(data.panel);
        });

        onEvent("pickerStateChanged", data => {
            this.state.pickerActive = !!data?.active;
            this.syncPickerButton();
            if (!this.state.pickerActive)
                this.elementsTree.previewNode(null);
        });

        onEvent("pickerEvent", data => {
            if (data?.type === "picked" && data.nodeId) {
                this.elementsTree.previewNode(null);
                this.selectPanel("elements");
                this.selectSidebar("styles");
                this.elementsTree.selectNode(data.nodeId, null, { reveal: true, focusRow: true });
            } else if (data?.type === "hovered" && data.nodeId) {
                // Show the row of the node under the pointer, as the picker's tooltip promises.
                this.elementsTree.previewNode(data.nodeId);
            }
        });

        onEvent("dockModeChanged", data => {
            if (data?.dockMode) {
                this.state.dockMode = data.dockMode;
                this.syncDockMenuUI();
            }
        });

        onEvent("targetDocumentChanged", data => {
            this.resetDocument(data);
            this.refreshDomTree();
        });

        onEvent("targetLoadFinished", () => {
            this.refreshDomTree();
        });

        onEvent("targetCrashed", () => {
            this.showBanner("Target tab crashed.", {
                actionLabel: "Reload & Reconnect",
                onAction: () => sendCommand("reconnectTarget").catch(err => this.showToast(err.message)),
            });
        });

        onEvent("targetRecovered", data => {
            this.resetDocument(data);
            this.clearBanner();
            this.refreshDomTree();
        });

        onEvent("consoleMessage", entry => this.drawers.addConsoleEntry(entry));
        onEvent("editJournalUpdated", journal => this.drawers.setEditJournal(journal));
        onEvent("domBreakpointsChanged", data => this.sidebars.setDomBreakpoints(data?.breakpoints || []));
        onEvent("debuggerPaused", pause => {
            this.toolboxPanels.pauseState = pause;
            this.selectPanel("sources", pause.frames?.[0]);
        });
        onEvent("debuggerResumed", () => this.toolboxPanels.setPaused(null));
        onEvent("networkRequestStarted", ev => this.toolboxPanels.addNetworkStart(ev));
        onEvent("networkResponseHeaders", ev => this.toolboxPanels.updateNetworkHeaders(ev));
        onEvent("networkRequestFinished", ev => this.toolboxPanels.finishNetwork(ev));
    }

    resetDocument(data) {
        this.state.documentGeneration = data.documentGeneration;
        this.state.targetUrl = data.url ?? this.state.targetUrl;
        this.state.activeGridOverlays.clear();
        this.state.activeFlexOverlays.clear();
        this.state.overlayColors.clear();
        this.state.selectedNodeId = null;
        this.state.selectedNode = null;
        this.state.selectedPseudo = null;
        this.state.lastComputedStyle = null;
        this.state.lastAppliedRules = null;
        this.elementsTree.resetDocument();
        this.sidebars.a11yTree = null;
        this.sidebars.a11yDetails = null;
        this.sidebars.computedData = {};
        this.sidebars.layoutMetrics = {};
        this.sidebars.usedFonts = [];
        this.stylesPane.resetDocument();
        this.stylesPane.setRules([]);
        this.toolboxPanels.activeSource = null;
        this.toolboxPanels.activeSourceText = "";
        this.toolboxPanels.sources = [];
        this.toolboxPanels.breakpoints.clear();
        this.toolboxPanels.pauseState = null;
        this.toolboxPanels.selectedNetworkId = null;
        if (!this.state.preserveConsoleLog)
            this.drawers.consoleEntries = [];
        if (!this.state.preserveNetworkLog)
            this.toolboxPanels.networkEntries = [];
        this.drawers.setEditJournal(null);
        this.drawers.quickStyleSource = null;
        this.selectSidebar(this.state.activeSidebar);
        this.selectPanel(this.state.activePanel);
        this.drawers.renderActive();
    }

    refreshDomTree() {
        const generation = this.state.documentGeneration;
        if (this.treeRequest?.generation === generation)
            return this.treeRequest.promise;
        const promise = sendCommand("getDomTree").then(res => {
            if (res?.tree && res.documentGeneration === this.state.documentGeneration)
                this.elementsTree.setTree(res.tree);
        }).catch(() => {}).finally(() => {
            if (this.treeRequest?.promise === promise)
                this.treeRequest = null;
        });
        this.treeRequest = { generation, promise };
        return promise;
    }

    onNodeSelected(nodeId, pseudo, node) {
        if (this.state.selectedNodeId !== nodeId || this.state.selectedPseudo !== pseudo) {
            this.state.lastComputedStyle = null;
            this.state.lastAppliedRules = null;
            this.sidebars.computedData = {};
            this.sidebars.layoutMetrics = {};
            this.sidebars.usedFonts = [];
            this.stylesPane.setRules([]);
        }
        this.state.selectedNodeId = nodeId;
        this.state.selectedPseudo = pseudo;
        this.state.selectedNode = node;
        if (!this.state.ghostSnapshot)
            this.refreshSelectedNodeProperties();
    }

    onGhostSnapshotActive(snapshot) {
        this.state.ghostSnapshot = snapshot;
        if (snapshot) {
            this.state.selectedNode = snapshot.node;
            if (snapshot.rules)
                this.stylesPane.setRules(snapshot.rules);
            if (snapshot.computed)
                this.sidebars.computedData = snapshot.computed;
            this.sidebars.layoutMetrics = {};
            this.sidebars.usedFonts = [];
            this.selectSidebar(this.state.activeSidebar);
        }
        this.stylesPane.render();
    }

    refreshSelectedNodeProperties() {
        const nodeId = this.state.selectedNodeId;
        if (!nodeId || this.state.ghostSnapshot)
            return;
        const pseudo = this.state.selectedPseudo || "";
        const ruleOptions = {
            inherited: true,
            filter: this.prefs.showUaStyles ? "ua" : "",
        };
        sendCommand("inspectNode", { nodeId, type: "appliedStyleRules", pseudoElement: pseudo, options: ruleOptions }).catch(() => {});
        sendCommand("inspectNode", { nodeId, type: "computedStyle", pseudoElement: pseudo }).catch(() => {});
        sendCommand("inspectNode", { nodeId, type: "layout", pseudoElement: pseudo }).catch(() => {});
        sendCommand("inspectNode", { nodeId, type: "usedFonts", pseudoElement: pseudo }).catch(() => {});

        if (this.state.activeSidebar === "listeners")
            this.sidebars.loadEventListeners();
        else if (this.state.activeSidebar === "properties")
            this.sidebars.loadProperties();
        else if (this.state.activeSidebar === "accessibility")
            this.sidebars.loadAccessibility();
        else if (this.state.activeSidebar === "layout")
            this.sidebars.renderLayout();
    }

    selectPanel(panelName, sourceLocation) {
        this.state.activePanel = panelName;
        document.body.dataset.panel = panelName;
        document.querySelectorAll("#panel-tabs button").forEach(b => {
            b.setAttribute("aria-selected", b.dataset.panel === panelName ? "true" : "false");
        });
        revealInStrip(document.getElementById("panel-tabs"), document.querySelector(`#panel-tabs button[data-panel="${panelName}"]`));
        document.querySelectorAll(".panel-view").forEach(p => {
            p.classList.toggle("active", p.id === `panel-${panelName}`);
        });

        const host = document.getElementById(`panel-${panelName}`);
        if (panelName === "console")
            this.drawers.renderConsole(host);
        else if (panelName === "sources")
            this.toolboxPanels.renderSources(host, sourceLocation);
        else if (panelName === "network")
            this.toolboxPanels.renderNetwork(host);
        else if (panelName === "application")
            this.toolboxPanels.renderApplication(host);
        else if (panelName === "performance")
            this.toolboxPanels.renderPerformance(host);
        else if (panelName === "memory")
            this.toolboxPanels.renderMemory(host);
        else if (panelName === "issues")
            this.toolboxPanels.renderIssues(host);
        else if (panelName === "audits")
            this.toolboxPanels.renderAudits(host);
    }

    selectSidebar(sidebarName) {
        this.state.activeSidebar = sidebarName;
        document.querySelectorAll("#sidebar-tabs button").forEach(b => {
            b.setAttribute("aria-selected", b.dataset.sidebar === sidebarName ? "true" : "false");
        });
        revealInStrip(document.getElementById("sidebar-tabs"), document.querySelector(`#sidebar-tabs button[data-sidebar="${sidebarName}"]`));
        document.querySelectorAll(".sidebar-view").forEach(v => {
            v.classList.toggle("active", v.id === `sidebar-${sidebarName}`);
        });

        if (sidebarName === "computed")
            this.sidebars.renderComputed();
        else if (sidebarName === "layout")
            this.sidebars.renderLayout();
        else if (sidebarName === "listeners")
            this.sidebars.loadEventListeners();
        else if (sidebarName === "breakpoints")
            this.sidebars.renderDomBreakpoints();
        else if (sidebarName === "properties")
            this.sidebars.loadProperties();
        else if (sidebarName === "accessibility")
            this.sidebars.loadAccessibility();
    }

    toggleGridOverlay(nodeId, color = "#2c7be5") {
        if (this.state.activeGridOverlays.has(nodeId)) {
            this.state.activeGridOverlays.delete(nodeId);
            sendCommand("clearGridHighlight", { nodeId }).catch(() => {});
        } else {
            this.state.activeGridOverlays.add(nodeId);
            this.state.overlayColors.set(nodeId, color);
            sendCommand("highlightGrid", { nodeId, options: { ...this.state.gridOverlayOptions, color } }).catch(() => {});
        }
        this.elementsTree.render();
        if (this.state.activeSidebar === "layout")
            this.sidebars.renderLayout();
    }

    refreshActiveGridOverlays() {
        for (const nodeId of this.state.activeGridOverlays) {
            const color = this.state.overlayColors.get(nodeId) || "#2c7be5";
            sendCommand("highlightGrid", { nodeId, options: { ...this.state.gridOverlayOptions, color } }).catch(() => {});
        }
    }

    toggleFlexOverlay(nodeId, color = "#2f855a") {
        if (this.state.activeFlexOverlays.has(nodeId)) {
            this.state.activeFlexOverlays.delete(nodeId);
            sendCommand("clearFlexboxHighlight", { nodeId }).catch(() => {});
        } else {
            this.state.activeFlexOverlays.add(nodeId);
            this.state.overlayColors.set(nodeId, color);
            sendCommand("highlightFlexbox", { nodeId, options: { color } }).catch(() => {});
        }
        this.elementsTree.render();
        if (this.state.activeSidebar === "layout")
            this.sidebars.renderLayout();
    }

    setDockMode(mode) {
        if (mode !== this.state.dockMode)
            this.state.previousDockMode = this.state.dockMode;
        this.state.dockMode = mode;
        this.syncDockMenuUI();
        sendCommand("setDockMode", { mode }).catch(() => {});
    }

    syncDockMenuUI() {
        document.querySelectorAll("#dock-menu button").forEach(btn => {
            btn.setAttribute("aria-checked", btn.dataset.dock === this.state.dockMode ? "true" : "false");
        });
    }

    syncPickerButton() {
        document.getElementById("btn-pick").setAttribute("aria-pressed", this.state.pickerActive ? "true" : "false");
    }

    toggleDeviceMode(force) {
        this.state.deviceModeActive = force !== undefined ? force : !this.state.deviceModeActive;
        document.getElementById("btn-device-mode").setAttribute("aria-pressed", this.state.deviceModeActive ? "true" : "false");
        document.getElementById("device-toolbar").classList.toggle("hidden", !this.state.deviceModeActive);
        if (this.state.deviceModeActive)
            this.applyDeviceMode();
        else
            sendEngineCommand("setDeviceMode", { enabled: false }).catch(() => {});
    }

    applyDeviceMode() {
        const width = parseInt(document.getElementById("device-width").value, 10) || 390;
        const height = parseInt(document.getElementById("device-height").value, 10) || 844;
        const dpr = parseFloat(document.getElementById("device-dpr").value) || 2;
        sendEngineCommand("setDeviceMode", { enabled: true, width, height, dpr }).catch(err => this.showToast(err.message));
    }

    openStyleSource(rule) {
        this.drawers.openStyleSource(rule.ruleId, this.stylesPane.formatRuleSource(rule), rule.sourceLine);
    }

    handleGlobalKeyDown(e) {
        if (e.defaultPrevented)
            return;
        const mod = e.metaKey || e.ctrlKey;
        if (mod && e.shiftKey && e.key.toLowerCase() === "p") {
            e.preventDefault();
            this.openCommandMenu();
        } else if (mod && e.shiftKey && e.key.toLowerCase() === "d") {
            e.preventDefault();
            this.setDockMode(this.state.previousDockMode || "bottom");
        } else if (mod && e.shiftKey && e.key.toLowerCase() === "m") {
            e.preventDefault();
            this.toggleDeviceMode();
        } else if (mod && e.key.toLowerCase() === "f" && this.state.activePanel === "elements") {
            e.preventDefault();
            this.elementsTree.openSearch();
        } else if (e.key === " " && this.state.pickerActive && !e.target.closest("input, textarea, select, button, [contenteditable]")) {
            e.preventDefault();
            this.state.loupeLatched = !this.state.loupeLatched;
            this.showToast(this.state.loupeLatched ? "Loupe latched" : "Loupe unlatched");
        } else if (e.key === "Escape") {
            const cmdDialog = document.getElementById("command-menu-dialog");
            const settingsDialog = document.getElementById("settings-dialog");
            if (cmdDialog.open) {
                cmdDialog.close();
            } else if (settingsDialog.open) {
                settingsDialog.close();
            } else if (!document.getElementById("context-menu").classList.contains("hidden")) {
                this.hideContextMenu();
                (this.contextMenuReturnFocus?.isConnected ? this.contextMenuReturnFocus : this.elementsTree.treeEl).focus();
            } else if (document.querySelector(".menu-dropdown:not(.hidden)")) {
                const open = document.querySelector(".menu-dropdown:not(.hidden)");
                this.closeAllDropdowns();
                open.parentElement.querySelector("[aria-haspopup]")?.focus();
            } else if (this.state.pickerActive) {
                sendCommand("setPickerActive", { active: false }).catch(() => {});
            } else {
                this.drawers.toggle();
            }
        }
    }

    openCommandMenu() {
        const dlg = document.getElementById("command-menu-dialog");
        const inp = document.getElementById("command-menu-input");
        inp.value = "";
        this.renderCommandMenu("");
        dlg.showModal();
        inp.focus();
    }

    renderCommandMenu(query) {
        const list = document.getElementById("command-menu-list");
        list.textContent = "";
        const commands = [
            { title: "Panel: Elements", run: () => this.selectPanel("elements") },
            { title: "Panel: Console", run: () => this.selectPanel("console") },
            { title: "Panel: Sources", run: () => this.selectPanel("sources") },
            { title: "Panel: Network", run: () => this.selectPanel("network") },
            { title: "Panel: Application", run: () => this.selectPanel("application") },
            { title: "Panel: Performance", run: () => this.selectPanel("performance") },
            { title: "Panel: Memory", run: () => this.selectPanel("memory") },
            { title: "Panel: Issues", run: () => this.selectPanel("issues") },
            { title: "Panel: Audits & CSS Overview", run: () => this.selectPanel("audits") },
            { title: "Drawer: Console", run: () => this.drawers.open("console") },
            { title: "Drawer: Changes / Flight Recorder", run: () => this.drawers.open("changes") },
            { title: "Drawer: Engine Lens", run: () => this.drawers.open("engine-lens") },
            { title: "Drawer: Animations", run: () => this.drawers.open("animations") },
            { title: "Drawer: Rendering", run: () => this.drawers.open("rendering") },
            { title: "Drawer: Sensors", run: () => this.drawers.open("sensors") },
            { title: "Dock: Right", run: () => this.setDockMode("right") },
            { title: "Dock: Bottom", run: () => this.setDockMode("bottom") },
            { title: "Dock: Left", run: () => this.setDockMode("left") },
            { title: "Dock: Separate Window", run: () => this.setDockMode("window") },
            { title: "Toggle Element Picker", run: () => sendCommand("setPickerActive", { active: !this.state.pickerActive }) },
            { title: "Toggle Device Mode", run: () => this.toggleDeviceMode() },
            { title: "Toggle Loupe Latch", run: () => { this.state.loupeLatched = !this.state.loupeLatched; } },
            { title: "Open Inspector Settings", run: () => this.openSettingsDialog() },
            { title: "Close Inspector", run: () => sendCommand("closeInspector") },
        ];
        const q = query.trim().toLowerCase();
        for (const cmd of commands) {
            if (q && !cmd.title.toLowerCase().includes(q))
                continue;
            const index = list.children.length;
            list.appendChild(el("button", {
                class: "command-item",
                type: "button",
                role: "option",
                tabindex: -1,
                id: `command-item-${index}`,
                text: cmd.title,
                onClick: () => {
                    document.getElementById("command-menu-dialog").close();
                    cmd.run();
                },
                onMousemove: () => this.setActiveCommand(index),
            }));
        }
        if (!list.children.length)
            list.appendChild(el("div", { class: "command-empty muted", text: "No matching commands" }));
        this.setActiveCommand(0);
    }

    setActiveCommand(index) {
        this.commandMenuIndex = index;
        const input = document.getElementById("command-menu-input");
        const items = [...document.querySelectorAll("#command-menu-list .command-item")];
        items.forEach((item, i) => {
            item.classList.toggle("active", i === index);
            item.setAttribute("aria-selected", i === index ? "true" : "false");
        });
        if (items[index]) {
            input.setAttribute("aria-activedescendant", items[index].id);
            items[index].scrollIntoView({ block: "nearest" });
        } else {
            input.removeAttribute("aria-activedescendant");
        }
    }

    // Menus are one keyboard stop: arrows, Home and End move between the enabled items.
    enableMenuKeyboard(menu) {
        menu.addEventListener("keydown", e => {
            const items = [...menu.querySelectorAll("button:not(:disabled)")];
            const index = items.indexOf(document.activeElement);
            let next = null;
            if (e.key === "ArrowDown")
                next = items[(index + 1) % items.length];
            else if (e.key === "ArrowUp")
                next = items[(index - 1 + items.length) % items.length];
            else if (e.key === "Home")
                next = items[0];
            else if (e.key === "End")
                next = items[items.length - 1];
            if (!next)
                return;
            e.preventDefault();
            next.focus();
        });
    }

    openSettingsDialog() {
        const dlg = document.getElementById("settings-dialog");
        const body = document.getElementById("settings-dialog-body");
        body.textContent = "";
        const opt = (label, key) => {
            const cb = el("input", {
                type: "checkbox",
                checked: !!this.prefs[key],
                onChange: () => {
                    this.prefs[key] = cb.checked;
                    this.elementsTree.render();
                    this.refreshSelectedNodeProperties();
                },
            });
            return el("label", { class: "inline-check" }, cb, label);
        };
        body.appendChild(opt("Show authored whitespace text nodes in Elements tree", "showWhitespace"));
        body.appendChild(opt("Show user-agent shadow DOM in Elements tree", "showUserAgentShadowDom"));
        body.appendChild(opt("Show User-Agent stylesheet rules in Styles", "showUaStyles"));
        body.appendChild(opt("Show APCA Lc contrast alongside WCAG 2 ratio", "useApcaContrast"));
        if (this.state.isPrivate)
            body.appendChild(el("p", { class: "muted", text: "Private inspection keeps preferences in session memory only." }));
        dlg.showModal();
    }

    runNamedCommand(cmd) {
        if (!cmd)
            return;
        if (cmd === "open-command-menu")
            this.openCommandMenu();
        else if (cmd === "open-settings")
            this.openSettingsDialog();
        else if (cmd === "close-inspector")
            sendCommand("closeInspector").catch(() => {});
        else if (cmd.startsWith("open-drawer:"))
            this.drawers.open(cmd.slice("open-drawer:".length));
    }

    setupDropdown(btnId, menuId) {
        const btn = document.getElementById(btnId);
        const menu = document.getElementById(menuId);
        btn.addEventListener("click", e => {
            e.stopPropagation();
            const wasHidden = menu.classList.contains("hidden");
            this.closeAllDropdowns();
            if (wasHidden) {
                menu.classList.remove("hidden");
                btn.setAttribute("aria-expanded", "true");
                // A pointer click leaves focus on the button; a keyboard activation (detail 0) moves into the menu.
                if (e.detail === 0)
                    (menu.querySelector('[aria-checked="true"]') || menu.querySelector("button"))?.focus();
            }
        });
    }

    closeAllDropdowns() {
        for (const [b, m] of [["btn-dock", "dock-menu"], ["btn-more", "more-menu"]]) {
            document.getElementById(m).classList.add("hidden");
            document.getElementById(b).setAttribute("aria-expanded", "false");
        }
    }

    showContextMenu(x, y, items) {
        const menu = document.getElementById("context-menu");
        menu.textContent = "";
        for (const item of items) {
            if (item.sep) {
                menu.appendChild(el("hr", { class: "menu-sep" }));
                continue;
            }
            menu.appendChild(el("button", {
                type: "button",
                role: "menuitem",
                disabled: !!item.disabled,
                text: item.label,
                onClick: () => {
                    this.hideContextMenu();
                    item.run();
                },
            }));
        }
        this.contextMenuReturnFocus = document.activeElement;
        menu.style.left = "0px";
        menu.style.top = "0px";
        menu.scrollTop = 0;
        menu.classList.remove("hidden");
        // The menu scrolls when it is taller than the frontend (a short bottom dock), and always stays inside it.
        const rect = menu.getBoundingClientRect();
        menu.style.left = `${Math.max(8, Math.min(x, window.innerWidth - rect.width - 8))}px`;
        menu.style.top = `${Math.max(8, Math.min(y, window.innerHeight - rect.height - 8))}px`;
        menu.querySelector("button:not(:disabled)")?.focus({ preventScroll: true });
    }

    hideContextMenu() {
        document.getElementById("context-menu").classList.add("hidden");
    }

    showBanner(text, { actionLabel, onAction } = {}) {
        const host = document.getElementById("banner-host");
        host.textContent = "";
        host.appendChild(el("span", { text }));
        if (actionLabel && onAction)
            host.appendChild(el("button", { class: "chip-btn", type: "button", text: actionLabel, onClick: onAction }));
        host.classList.remove("hidden");
    }

    clearBanner() {
        document.getElementById("banner-host").classList.add("hidden");
    }

    showToast(text) {
        const pill = document.getElementById("status-pill");
        pill.textContent = text;
        pill.classList.remove("hidden");
        announce(text);
        clearTimeout(this.toastTimer);
        this.toastTimer = setTimeout(() => pill.classList.add("hidden"), 3200);
    }

    setupSplitters() {
        // Between the DOM tree and the Elements sidebar: a column split, or a row split when the frontend is narrow.
        const splitter = document.getElementById("elements-splitter");
        const treePane = document.getElementById("elements-tree-pane");
        const sidePane = document.getElementById("elements-sidebar-pane");
        const splitContainer = document.getElementById("elements-split");
        let treeRatio = null;
        const isStacked = () => getComputedStyle(splitContainer).flexDirection === "column";
        const applyTreeRatio = ratio => {
            treeRatio = Math.min(0.8, Math.max(0.2, ratio));
            treePane.style.flex = `0 0 ${treeRatio * 100}%`;
            sidePane.style.flex = "1 1 0";
            splitter.setAttribute("aria-valuenow", String(Math.round(treeRatio * 100)));
        };
        splitter.setAttribute("aria-valuemin", "20");
        splitter.setAttribute("aria-valuemax", "80");
        const syncOrientation = () => splitter.setAttribute("aria-orientation", isStacked() ? "horizontal" : "vertical");
        new ResizeObserver(syncOrientation).observe(splitContainer);
        this.makeSplitterDraggable(splitter, e => {
            const rect = splitContainer.getBoundingClientRect();
            applyTreeRatio(isStacked() ? (e.clientY - rect.top) / rect.height : (e.clientX - rect.left) / rect.width);
        });
        splitter.addEventListener("keydown", e => {
            const step = e.shiftKey ? 0.1 : 0.02;
            const current = treeRatio ?? treePane.getBoundingClientRect()[isStacked() ? "height" : "width"]
                / splitContainer.getBoundingClientRect()[isStacked() ? "height" : "width"];
            const decrease = isStacked() ? "ArrowUp" : "ArrowLeft";
            const increase = isStacked() ? "ArrowDown" : "ArrowRight";
            if (e.key === decrease)
                applyTreeRatio(current - step);
            else if (e.key === increase)
                applyTreeRatio(current + step);
            else if (e.key === "Home")
                applyTreeRatio(0.2);
            else if (e.key === "End")
                applyTreeRatio(0.8);
            else
                return;
            e.preventDefault();
        });

        // Above the drawer.
        const drawerSplitter = document.getElementById("drawer-splitter");
        const drawerEl = document.getElementById("inspector-drawer");
        const applyDrawerHeight = height => {
            const clamped = Math.round(Math.min(window.innerHeight - 160, Math.max(120, height)));
            drawerEl.style.height = `${clamped}px`;
            drawerSplitter.setAttribute("aria-valuenow", String(clamped));
        };
        this.makeSplitterDraggable(drawerSplitter, e => applyDrawerHeight(drawerEl.getBoundingClientRect().bottom - e.clientY));
        drawerSplitter.addEventListener("keydown", e => {
            const step = e.shiftKey ? 60 : 12;
            const current = drawerEl.getBoundingClientRect().height;
            if (e.key === "ArrowUp")
                applyDrawerHeight(current + step);
            else if (e.key === "ArrowDown")
                applyDrawerHeight(current - step);
            else
                return;
            e.preventDefault();
        });
    }

    // A drag that starts on the splitter follows the pointer until it is released or cancelled. Text under the pointer
    // is not selected along the way.
    makeSplitterDraggable(splitter, onMove) {
        let pointerId = null;
        const stop = () => {
            pointerId = null;
            splitter.classList.remove("dragging");
            document.body.classList.remove("splitter-dragging");
            document.body.style.removeProperty("cursor");
        };
        splitter.addEventListener("pointerdown", e => {
            if (e.button !== 0)
                return;
            e.preventDefault();
            pointerId = e.pointerId;
            try {
                splitter.setPointerCapture(e.pointerId);
            } catch {
                // A pointer that is no longer down cannot be captured; the drag still follows it over the splitter.
            }
            splitter.classList.add("dragging");
            document.body.classList.add("splitter-dragging");
            // The pointer keeps the resize cursor when it outruns the thin splitter.
            document.body.style.cursor = getComputedStyle(splitter).cursor;
            splitter.focus({ preventScroll: true });
        });
        splitter.addEventListener("pointermove", e => {
            if (e.pointerId === pointerId)
                onMove(e);
        });
        splitter.addEventListener("pointerup", stop);
        splitter.addEventListener("pointercancel", stop);
        splitter.addEventListener("lostpointercapture", stop);
    }
}

window.ladyflockInspector = new InspectorApp();
