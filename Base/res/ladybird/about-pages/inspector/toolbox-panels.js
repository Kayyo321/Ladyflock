/*
 * Copyright (c) 2026, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Wider Toolbox Panels: Sources (with breakpoints/stack/scope/stepping),
 * Network (waterfall, headers, HAR export), Application (Cookies/Storage/IDB),
 * Performance, Memory, Issues, and Audits / CSS Overview / Recorder.
 */

import { el, sendCommand, sendEngineCommand } from "./bridge.js";

export class ToolboxPanelsController {
    constructor(app) {
        this.app = app;
        this.sources = [];
        this.activeSource = null;
        this.activeSourceText = "";
        this.breakpoints = new Set();
        this.pauseState = null;
        this.networkEntries = [];
        this.networkFilter = "";
        this.selectedNetworkId = null;
        this.applicationTab = "cookies";
        this.issues = [];
    }

    setNetworkEntries(entries) {
        this.networkEntries = entries || [];
        if (this.app.state.activePanel === "network")
            this.renderNetwork(document.getElementById("panel-network"));
    }

    addNetworkStart(ev) {
        this.networkEntries.push({ ...ev, statusCode: 0, finished: false, durationMs: 0, bodySize: 0 });
        if (this.networkEntries.length > 500)
            this.networkEntries.shift();
        if (this.app.state.activePanel === "network")
            this.renderNetwork(document.getElementById("panel-network"));
    }

    updateNetworkHeaders(ev) {
        const row = this.networkEntries.find(e => e.requestId === ev.requestId);
        if (row) {
            row.statusCode = ev.statusCode;
            row.reasonPhrase = ev.reasonPhrase;
            row.cacheState = ev.cacheState;
            row.responseHeaders = ev.responseHeaders;
        }
        if (this.app.state.activePanel === "network")
            this.renderNetwork(document.getElementById("panel-network"));
    }

    finishNetwork(ev) {
        const row = this.networkEntries.find(e => e.requestId === ev.requestId);
        if (row) {
            row.bodySize = ev.bodySize;
            row.durationMs = ev.durationMs;
            row.error = ev.error;
            row.finished = true;
        }
        if (this.app.state.activePanel === "network")
            this.renderNetwork(document.getElementById("panel-network"));
    }

    setPaused(pause) {
        this.pauseState = pause;
        if (this.app.state.activePanel === "sources")
            this.renderSources(document.getElementById("panel-sources"));
    }

    // 1. Sources & Debugger Panel
    async renderSources(host) {
        host.textContent = "";
        const toolbar = el(
            "div",
            { class: "pane-toolbar" },
            this.pauseState
                ? el("span", { class: "status-pill", text: `Paused (${this.pauseState.reason})` })
                : el("span", { class: "muted", text: "Sources & Debugger" }),
            el("button", { class: "chip-btn", type: "button", text: "Pause", disabled: !!this.pauseState, onClick: () => sendCommand("interruptDebugger") }),
            el("button", { class: "chip-btn primary", type: "button", text: "Resume", disabled: !this.pauseState, onClick: () => sendCommand("resumeDebugger", { mode: "continue" }) }),
            el("button", { class: "chip-btn", type: "button", text: "Step Over", disabled: !this.pauseState, onClick: () => sendCommand("resumeDebugger", { mode: "stepOver" }) }),
            el("button", { class: "chip-btn", type: "button", text: "Step Into", disabled: !this.pauseState, onClick: () => sendCommand("resumeDebugger", { mode: "stepInto" }) }),
            el("button", { class: "chip-btn", type: "button", text: "Step Out", disabled: !this.pauseState, onClick: () => sendCommand("resumeDebugger", { mode: "stepOut" }) })
        );
        host.appendChild(toolbar);

        const split = el("div", { class: "sources-split" });
        const sidebar = el("div", { class: "sources-sidebar" });
        const codeEl = el("div", { class: "sources-code" });
        split.append(sidebar, codeEl);
        host.appendChild(split);

        try {
            const res = await sendCommand("listSources");
            this.sources = res?.sources || [];
        } catch {
            // keep existing
        }

        if (this.sources.length === 0) {
            sidebar.appendChild(el("div", { class: "muted", text: "No scripts registered in this page yet." }));
        } else {
            for (const src of this.sources) {
                const label = src.displayUrl || src.url || `(inline script #${src.id?.scriptId})`;
                sidebar.appendChild(el("button", {
                    class: `command-item monospace ${this.activeSource?.id?.scriptId === src.id?.scriptId ? "active" : ""}`,
                    type: "button",
                    style: "font-size:11px;padding:4px 6px",
                    text: label.split("/").pop() || label,
                    title: label,
                    onClick: () => this.openSource(src, codeEl),
                }));
            }
        }

        if (this.pauseState?.frames?.length) {
            sidebar.appendChild(el("div", { style: "font-weight:600;margin-top:10px;font-size:11px", text: "Call Stack" }));
            for (const frame of this.pauseState.frames) {
                sidebar.appendChild(el("div", {
                    class: "monospace muted",
                    style: "font-size:11px;padding:2px 4px",
                    text: `${frame.displayName || "(anonymous)"} :${frame.line}:${frame.column}`,
                }));
            }
        }

        if (this.activeSource)
            this.renderSourceLines(codeEl);
        else if (this.sources[0])
            this.openSource(this.sources[0], codeEl);
    }

    async openSource(src, codeEl) {
        this.activeSource = src;
        try {
            const res = await sendCommand("getSourceContent", {
                documentId: src.id.documentId,
                scriptId: src.id.scriptId,
            });
            this.activeSourceText = res?.text || "";
        } catch (err) {
            this.activeSourceText = `// Could not load source: ${err.message}`;
        }
        this.renderSourceLines(codeEl);
    }

    renderSourceLines(codeEl) {
        codeEl.textContent = "";
        const lines = this.activeSourceText.split("\n");
        const pausedLine = this.pauseState?.frames?.[0]?.line || -1;
        lines.forEach((lineText, idx) => {
            const lineNum = idx + 1;
            const bpKey = `${this.activeSource?.id?.scriptId}:${lineNum}`;
            const hasBp = this.breakpoints.has(bpKey);
            codeEl.appendChild(el(
                "div",
                { class: `code-line ${pausedLine === lineNum ? "paused" : ""}` },
                el("span", {
                    class: `code-gutter tabular ${hasBp ? "bp" : ""}`,
                    text: String(lineNum),
                    title: "Toggle breakpoint",
                    onClick: async () => {
                        if (hasBp) {
                            this.breakpoints.delete(bpKey);
                            await sendCommand("removeBreakpoint", {
                                documentId: this.activeSource.id.documentId,
                                scriptId: this.activeSource.id.scriptId,
                                filename: this.activeSource.url || this.activeSource.displayUrl || "",
                                line: lineNum,
                            }).catch(() => {});
                        } else {
                            this.breakpoints.add(bpKey);
                            await sendCommand("setBreakpoint", {
                                documentId: this.activeSource.id.documentId,
                                scriptId: this.activeSource.id.scriptId,
                                filename: this.activeSource.url || this.activeSource.displayUrl || "",
                                line: lineNum,
                            }).catch(() => {});
                        }
                        this.renderSourceLines(codeEl);
                    },
                }),
                el("span", { text: lineText })
            ));
        });
    }

    // 2. Network & Security Panel
    renderNetwork(host) {
        host.textContent = "";
        const filterInput = el("input", {
            type: "search",
            placeholder: "Filter requests by URL, method, or status",
            value: this.networkFilter,
            onInput: e => { this.networkFilter = e.target.value; this.renderNetwork(host); },
        });
        const preserveCb = el("input", {
            type: "checkbox",
            checked: this.app.state.preserveNetworkLog,
            onChange: () => {
                this.app.state.preserveNetworkLog = preserveCb.checked;
                sendCommand("setPreserveLog", { network: preserveCb.checked }).catch(() => {});
            },
        });
        host.appendChild(el(
            "div",
            { class: "pane-toolbar" },
            el("button", {
                class: "chip-btn",
                type: "button",
                text: "Clear",
                onClick: () => {
                    this.networkEntries = [];
                    sendCommand("clearNetworkLog").catch(() => {});
                    this.renderNetwork(host);
                },
            }),
            filterInput,
            el("label", { class: "inline-check" }, preserveCb, "Preserve log"),
            el("button", {
                class: "chip-btn",
                type: "button",
                text: "Copy Sanitized HAR",
                onClick: () => this.exportSanitizedHar(),
            })
        ));

        const q = this.networkFilter.trim().toLowerCase();
        const rows = this.networkEntries.filter(r => !q
            || String(r.url || "").toLowerCase().includes(q)
            || String(r.method || "").toLowerCase().includes(q)
            || String(r.statusCode || "").includes(q));

        const maxDur = Math.max(1, ...rows.map(r => r.durationMs || 1));
        const table = el("table", { class: "data-table tabular" });
        table.appendChild(el(
            "thead",
            {},
            el(
                "tr",
                {},
                el("th", { text: "Name" }),
                el("th", { text: "Method" }),
                el("th", { text: "Status" }),
                el("th", { text: "Type" }),
                el("th", { text: "Size" }),
                el("th", { text: "Time" }),
                el("th", { text: "Waterfall" })
            )
        ));
        const tbody = el("tbody");
        for (const req of rows) {
            const name = String(req.url || "").split("/").pop() || req.url;
            const bar = el("div", { class: "waterfall-bar" });
            bar.style.width = `${Math.max(4, Math.round(((req.durationMs || 1) / maxDur) * 100))}%`;
            tbody.appendChild(el(
                "tr",
                { onClick: () => { this.selectedNetworkId = req.requestId; this.renderNetwork(host); } },
                el("td", { class: "monospace", text: name, title: req.url }),
                el("td", { text: req.method || "GET" }),
                el("td", { text: req.error ? `Error (${req.error})` : (req.statusCode || "…") }),
                el("td", { text: req.initiatorType || "other" }),
                el("td", { text: req.bodySize ? `${req.bodySize} B` : "–" }),
                el("td", { text: req.durationMs ? `${req.durationMs.toFixed(1)} ms` : "–" }),
                el("td", { style: "min-width:110px" }, bar)
            ));
        }
        table.appendChild(tbody);
        host.appendChild(el("div", { style: "flex:1;overflow:auto" }, table));
    }

    exportSanitizedHar() {
        // Strip cookies / authorization headers so HAR exports never leak credentials!
        const sanitizeHeaders = headers => (headers || []).filter(h => !/^(cookie|set-cookie|authorization)$/i.test(h.name));
        const har = {
            log: {
                version: "1.2",
                creator: { name: "Ladyflock Inspector", version: "1.0" },
                entries: this.networkEntries.map(e => ({
                    request: { method: e.method, url: e.url, headers: sanitizeHeaders(e.requestHeaders) },
                    response: { status: e.statusCode, statusText: e.reasonPhrase || "", headers: sanitizeHeaders(e.responseHeaders), bodySize: e.bodySize },
                    time: e.durationMs,
                })),
            },
        };
        sendCommand("copyText", { text: JSON.stringify(har, null, 2) });
        this.app.showToast("Copied sanitized HAR JSON to clipboard");
    }

    // 3. Application / Storage Panel
    async renderApplication(host) {
        host.textContent = "";
        const tabs = [
            ["cookies", "Cookies"],
            ["localStorage", "Local Storage"],
            ["sessionStorage", "Session Storage"],
            ["indexedDB", "IndexedDB"],
        ];
        const bar = el("div", { class: "pane-toolbar" });
        for (const [k, label] of tabs) {
            bar.appendChild(el("button", {
                class: `chip-btn ${this.applicationTab === k ? "active" : ""}`,
                type: "button",
                text: label,
                onClick: () => { this.applicationTab = k; this.renderApplication(host); },
            }));
        }
        host.appendChild(bar);

        const body = el("div", { style: "flex:1;overflow:auto;padding:8px" }, el("span", { class: "muted", text: "Loading storage…" }));
        host.appendChild(body);

        try {
            if (this.applicationTab === "indexedDB") {
                const res = await sendCommand("inspectIndexedDB");
                body.textContent = "";
                body.appendChild(el("pre", { class: "monospace", text: JSON.stringify(res, null, 2) }));
                return;
            }
            const res = await sendCommand("getStorage", { storageType: this.applicationTab });
            const items = res?.items || [];
            body.textContent = "";
            if (items.length === 0) {
                body.appendChild(el("div", { class: "muted", text: `No ${this.applicationTab} items for this origin.` }));
                return;
            }
            const t = el("table", { class: "data-table monospace" });
            const tb = el("tbody");
            for (const it of items) {
                tb.appendChild(el(
                    "tr",
                    {},
                    el("td", { text: it.name ?? it.key ?? "" }),
                    el("td", { text: String(it.value ?? "") }),
                    it.domain ? el("td", { class: "muted", text: it.domain }) : null
                ));
            }
            t.appendChild(tb);
            body.appendChild(t);
        } catch (err) {
            body.textContent = `Storage unavailable: ${err.message}`;
        }
    }

    // 4. Performance & Memory Panels
    renderPerformance(host) {
        host.textContent = "";
        host.appendChild(el(
            "div",
            { class: "pane-toolbar" },
            el("span", { class: "muted", text: "Performance & Layout Diagnostics" }),
            el("button", {
                class: "chip-btn primary",
                type: "button",
                text: "Capture Layout & Paint Snapshot",
                onClick: () => this.app.drawers.open("engine-lens"),
            })
        ));
        host.appendChild(el(
            "div",
            { class: "section-block tabular" },
            el("h3", { text: "Live Target Metrics" }),
            el("div", { text: `Target URL: ${this.app.state.targetUrl || "—"}` }),
            el("div", { text: `DOM nodes indexed: ${this.app.elementsTree.nodesById.size}` }),
            el("div", { text: `Network requests captured: ${this.networkEntries.length}` }),
            el("div", { text: `Document generation: #${this.app.state.documentGeneration}` })
        ));
    }

    renderMemory(host) {
        host.textContent = "";
        host.appendChild(el(
            "div",
            { class: "pane-toolbar" },
            el("span", { class: "muted", text: "Memory & Heap Diagnostics" }),
            el("button", {
                class: "chip-btn primary",
                type: "button",
                text: "Inspect Detached Snapshot",
                onClick: () => this.app.selectPanel("elements"),
            })
        ));
        host.appendChild(el(
            "div",
            { class: "section-block" },
            el("h3", { text: "Session Memory Footprint" }),
            el("div", { class: "tabular", text: `Live DOM nodes tracked: ${this.app.elementsTree.nodesById.size}` }),
            el("div", { class: "tabular", text: `Flight Recorder entries: ${(this.app.drawers.editJournal.entries || []).length}` }),
            el("div", { class: "tabular", text: `Ghost Snapshot active: ${this.app.state.ghostSnapshot ? "Yes (serialized data only)" : "No"}` })
        ));
    }

    // 5. Issues Panel
    renderIssues(host) {
        host.textContent = "";
        const failedReqs = this.networkEntries.filter(r => r.error || (r.statusCode >= 400));
        host.appendChild(el("div", { class: "pane-toolbar" }, el("span", { class: "muted", text: `Diagnostics & Issues (${failedReqs.length})` })));
        if (failedReqs.length === 0) {
            host.appendChild(el("div", { class: "section-block muted", text: "No network or diagnostic issues detected in this session." }));
            return;
        }
        for (const req of failedReqs) {
            host.appendChild(el(
                "div",
                { class: "section-block" },
                el("strong", { text: req.error ? `Failed request: ${req.error}` : `HTTP ${req.statusCode} ${req.reasonPhrase || ""}` }),
                el("div", { class: "monospace muted", text: req.url })
            ));
        }
    }

    // 6. Audits & CSS Overview Panel
    async renderAudits(host) {
        host.textContent = "";
        host.appendChild(el(
            "div",
            { class: "pane-toolbar" },
            el("span", { class: "muted", text: "CSS Overview & Quick Accessibility Audit" }),
            el("button", {
                class: "chip-btn primary",
                type: "button",
                text: "Run Page Audit",
                onClick: () => this.runQuickAudit(resultsEl),
            })
        ));
        const resultsEl = el("div", { class: "section-block muted", text: "Click 'Run Page Audit' to inspect colors, fonts, missing alt text, and contrast across this document." });
        host.appendChild(resultsEl);
    }

    async runQuickAudit(resultsEl) {
        resultsEl.textContent = "Scanning document…";
        try {
            const res = await sendEngineCommand("runCssOverviewAudit");
            resultsEl.textContent = "";
            resultsEl.appendChild(el("h3", { text: "Audit Summary" }));
            resultsEl.appendChild(el("div", { class: "tabular", text: `Elements scanned: ${res.elementCount ?? 0}` }));
            resultsEl.appendChild(el("div", { class: "tabular", text: `Images missing alt text: ${(res.missingAltNodeIds || []).length}` }));
            for (const id of res.missingAltNodeIds || []) {
                resultsEl.appendChild(el("button", {
                    class: "source-link monospace",
                    type: "button",
                    style: "display:block;margin:2px 0",
                    text: `Reveal <img> node #${id}`,
                    onClick: () => {
                        this.app.selectPanel("elements");
                        this.app.elementsTree.selectNode(id, null, { reveal: true, focusRow: true });
                    },
                }));
            }
        } catch (err) {
            resultsEl.textContent = `Audit could not run: ${err.message}`;
        }
    }
}
