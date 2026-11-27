/*
 * Copyright (c) 2026, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Wider Toolbox Panels: Sources (with breakpoints/stack/scope/stepping),
 * Network (durations, headers, JSON export), Application (Cookies/Storage/IDB),
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
            this.renderNetworkRows(document.getElementById("network-rows"));
    }

    addNetworkStart(ev) {
        this.networkEntries.push({ ...ev, statusCode: 0, finished: false, durationMs: 0, bodySize: 0 });
        if (this.networkEntries.length > 500)
            this.networkEntries.shift();
        if (this.app.state.activePanel === "network")
            this.renderNetworkRows(document.getElementById("network-rows"));
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
            this.renderNetworkRows(document.getElementById("network-rows"));
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
            this.renderNetworkRows(document.getElementById("network-rows"));
    }

    setPaused(pause) {
        this.pauseState = pause;
        if (this.app.state.activePanel === "sources")
            this.renderSources(document.getElementById("panel-sources"));
    }

    // 1. Sources & Debugger Panel
    async renderSources(host, location) {
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

        const generation = this.app.state.documentGeneration;
        try {
            const res = await sendCommand("listSources");
            if (!sidebar.isConnected || generation !== this.app.state.documentGeneration)
                return;
            this.sources = res?.sources || [];
        } catch (err) {
            sidebar.textContent = err.message;
            return;
        }

        if (this.sources.length === 0) {
            sidebar.appendChild(el("div", { class: "muted", text: "No scripts registered in this page yet." }));
        } else {
            for (const src of this.sources) {
                const label = src.displayUrl || src.url || `(inline script #${src.id?.scriptId})`;
                sidebar.appendChild(el("button", {
                    class: `command-item monospace sources-item ${this.activeSource?.id?.scriptId === src.id?.scriptId ? "active" : ""}`,
                    type: "button",
                    text: label.split("/").pop() || label,
                    title: label,
                    onClick: () => this.openSource(src, codeEl),
                }));
            }
        }

        if (this.pauseState?.frames?.length) {
            sidebar.appendChild(el("div", { class: "sources-heading", text: "Call Stack" }));
            for (const frame of this.pauseState.frames) {
                sidebar.appendChild(el("button", {
                    type: "button",
                    class: "command-item monospace sources-frame",
                    text: `${frame.displayName || "(anonymous)"} :${frame.line}:${frame.column}`,
                    onClick: () => this.openSource(frame.source, codeEl, frame.line),
                }));
            }
        }

        if (location?.source?.id)
            this.openSource(location.source, codeEl, location.line);
        else if (this.activeSource)
            this.renderSourceLines(codeEl);
        else if (this.sources[0])
            this.openSource(this.sources[0], codeEl);
    }

    async openSource(src, codeEl, line) {
        this.activeSource = src;
        this.activeSourceText = "";
        codeEl.textContent = "Loading source…";
        try {
            const res = await sendCommand("getSourceContent", {
                documentId: src.id.documentId,
                scriptId: src.id.scriptId,
            });
            if (this.activeSource !== src || !codeEl.isConnected)
                return;
            this.activeSourceText = res?.text || "";
        } catch (err) {
            if (this.activeSource === src)
                codeEl.textContent = err.message;
            return;
        }
        this.renderSourceLines(codeEl);
        if (Number.isInteger(line)) {
            const row = codeEl.querySelector(`[data-line="${line}"]`);
            if (row) {
                row.classList.add("source-location");
                row.scrollIntoView({ block: "center" });
            }
        }
    }

    renderSourceLines(codeEl) {
        codeEl.textContent = "";
        const lines = this.activeSourceText.split("\n");
        const source = this.activeSource;
        const frame = this.pauseState?.frames?.[0];
        const pausedLine = frame?.source?.id?.documentId === source.id.documentId
            && frame?.source?.id?.scriptId === source.id.scriptId ? frame.line : null;
        lines.forEach((lineText, idx) => {
            const lineNum = idx + Math.max(1, source.sourceStartLine || 1);
            const bpKey = `${source.id.documentId}:${source.id.scriptId}:${lineNum}`;
            const hasBp = this.breakpoints.has(bpKey);
            codeEl.appendChild(el(
                "div",
                { class: `code-line ${pausedLine === lineNum ? "paused" : ""}`, dataset: { line: lineNum } },
                el("button", {
                    type: "button",
                    class: `code-gutter tabular ${hasBp ? "bp" : ""}`,
                    text: String(lineNum),
                    "aria-label": `Breakpoint on line ${lineNum}`,
                    "aria-pressed": String(hasBp),
                    onClick: async e => {
                        const button = e.currentTarget;
                        button.disabled = true;
                        try {
                            await sendCommand(hasBp ? "removeBreakpoint" : "setBreakpoint", {
                                ...source.id,
                                filename: source.url || source.displayUrl || "",
                                line: lineNum,
                            });
                            if (hasBp)
                                this.breakpoints.delete(bpKey);
                            else
                                this.breakpoints.add(bpKey);
                            if (this.activeSource === source) {
                                button.classList.toggle("bp", !hasBp);
                                this.renderSourceLines(codeEl);
                                codeEl.querySelectorAll(".code-gutter")[idx]?.focus();
                            }
                        } catch (err) {
                            this.app.showToast(err.message);
                            button.disabled = false;
                        }
                    },
                }),
                el("span", { class: "source-line-text", text: lineText })
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
            "aria-label": "Filter network requests",
            onInput: e => { this.networkFilter = e.target.value; this.renderNetworkRows(rowsHost); },
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
                text: "Copy network log",
                onClick: () => this.copyNetworkLog(),
            })
        ));

        const rowsHost = el("div", { id: "network-rows", class: "scroll-fill" });
        host.appendChild(rowsHost);
        this.renderNetworkRows(rowsHost);
    }

    renderNetworkRows(host) {
        if (!host)
            return;
        host.textContent = "";
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
                el("th", { text: "Relative duration" })
            )
        ));
        const tbody = el("tbody");
        for (const req of rows) {
            const name = String(req.url || "").split("/").pop() || req.url;
            const bar = el("div", { class: "waterfall-bar" });
            bar.style.width = `${Math.max(4, Math.round(((req.durationMs || 1) / maxDur) * 100))}%`;
            tbody.appendChild(el(
                "tr",
                {},
                el("td", {}, el("button", { class: "source-link monospace", type: "button", text: name, title: req.url,
                    onClick: async () => {
                        this.selectedNetworkId = req.requestId;
                        try {
                            const log = await sendCommand("getNetworkLog");
                            if (this.selectedNetworkId !== req.requestId)
                                return;
                            const snapshot = log.entries.find(entry => entry.requestId === req.requestId);
                            if (snapshot)
                                req.requestHeaders = snapshot.requestHeaders;
                        } catch (err) {
                            this.app.showToast(err.message);
                        }
                        this.renderNetworkRows(host);
                    },
                })),
                el("td", { text: req.method || "GET" }),
                el("td", { text: req.error ? `Error (${req.error})` : (req.statusCode || "…") }),
                el("td", { text: req.initiatorType || "other" }),
                el("td", { text: req.bodySize ? `${req.bodySize} B` : "–" }),
                el("td", { text: req.durationMs ? `${req.durationMs.toFixed(1)} ms` : "–" }),
                el("td", { class: "waterfall-cell" }, bar)
            ));
        }
        table.appendChild(tbody);
        host.appendChild(table);
        const selected = rows.find(row => row.requestId === this.selectedNetworkId);
        if (selected) {
            const detail = el("section", { class: "section-block" }, el("h3", { text: selected.url }));
            for (const [label, headers] of [["Request headers", selected.requestHeaders], ["Response headers", selected.responseHeaders]]) {
                detail.appendChild(el("h4", { text: label }));
                detail.appendChild(el("pre", { text: headers?.map(header => `${header.name}: ${header.value}`).join("\n") || "No headers captured" }));
            }
            host.appendChild(detail);
        }
    }

    async copyNetworkLog() {
        const sanitizeHeaders = headers => (headers || []).filter(h => /^(content-length|content-type|cache-control)$/i.test(h.name));
        const sanitizeUrl = raw => {
            try {
                const url = new URL(raw);
                return ["http:", "https:"].includes(url.protocol) ? url.origin + url.pathname : url.protocol;
            } catch {
                return "";
            }
        };
        try {
            const { entries } = await sendCommand("getNetworkLog");
            const log = entries.map(e => ({
                request: { method: e.method, url: sanitizeUrl(e.url), headers: sanitizeHeaders(e.requestHeaders) },
                response: { status: e.statusCode, headers: sanitizeHeaders(e.responseHeaders), bodySize: e.bodySize },
                durationMs: e.durationMs,
            }));
            await sendCommand("copyText", { text: JSON.stringify(log, null, 2) });
            this.app.showToast("Copied network log");
        } catch (err) {
            this.app.showToast(err.message);
        }
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

        const body = el("div", { class: "scroll-fill padded" }, el("span", { class: "muted", text: "Loading storage…" }));
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
                const label = tabs.find(([key]) => key === this.applicationTab)?.[1] ?? this.applicationTab;
                body.appendChild(el("div", { class: "muted", text: `No ${label.toLowerCase()} for this origin.` }));
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
                text: "Open Engine Lens",
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
            el("span", { class: "muted", text: "Session diagnostics" }),
            el("button", {
                class: "chip-btn primary",
                type: "button",
                text: "Inspect Detached Snapshot",
                disabled: !this.app.state.ghostSnapshot,
                onClick: () => this.app.selectPanel("elements"),
            })
        ));
        host.appendChild(el(
            "div",
            { class: "section-block" },
            el("h3", { text: "Inspector session counts" }),
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
            host.appendChild(el("div", { class: "section-block muted", text: "No failed requests in the captured network log." }));
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
        const resultsEl = el("div", { class: "section-block muted", text: "Inspect document colors, fonts, and accessibility issues." });
        host.appendChild(resultsEl);
    }

    async runQuickAudit(resultsEl) {
        resultsEl.textContent = "Scanning document…";
        try {
            const res = await sendEngineCommand("runCssOverviewAudit");
            resultsEl.textContent = "";
            resultsEl.appendChild(el("h3", { text: "Audit Summary" }));
            resultsEl.appendChild(el("div", { class: "tabular", text: `Elements scanned: ${res.elementCount ?? 0}` }));
            for (const [label, values] of [["Text colors", res.colors], ["Background colors", res.backgroundColors], ["Font families", res.fonts]]) {
                if (values?.length)
                    resultsEl.appendChild(el("details", {}, el("summary", { text: label }), el("ul", {}, ...values.map(text => el("li", { text })))));
            }
            const issues = res.issues || [];
            resultsEl.appendChild(el("h3", { text: `Accessibility findings (${issues.length}${issues.length === 50 ? "+" : ""})` }));
            for (const issue of issues) {
                resultsEl.appendChild(el("button", {
                    class: "source-link monospace",
                    type: "button",
                    text: issue.message,
                    onClick: () => {
                        this.app.selectPanel("elements");
                        this.app.elementsTree.selectNode(issue.nodeId, null, { reveal: true, focusRow: true });
                    },
                }));
            }
        } catch (err) {
            resultsEl.textContent = `Audit could not run: ${err.message}`;
        }
    }
}
