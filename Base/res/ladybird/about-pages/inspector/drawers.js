/*
 * Copyright (c) 2026, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Inspector Drawers: Console, Changes / Flight Recorder, Engine Lens,
 * Quick Source, Animations, Rendering, and Sensors.
 */

import { el, sendCommand, sendEngineCommand } from "./bridge.js";

export class DrawersController {
    constructor(app) {
        this.app = app;
        this.drawerEl = document.getElementById("inspector-drawer");
        this.splitterEl = document.getElementById("drawer-splitter");
        this.bodyEl = document.getElementById("drawer-body");
        this.activeDrawer = "console";
        this.isOpen = false;
        this.consoleEntries = [];
        this.consoleFilter = "";
        this.consoleLevel = "all";
        this.consoleHistory = [];
        this.consoleHistoryIdx = -1;
        this.editJournal = { entries: [], truncated: false };
        this.engineLensKind = "layout";
        this.engineLensText = "";
        this.quickStyleSource = null;
        this.animations = [];
        this.colorScheme = "auto";
        this.sensorState = { latitude: "", longitude: "" };

        document.querySelectorAll("#drawer-tabs button").forEach(btn => {
            btn.addEventListener("click", () => this.open(btn.dataset.drawer));
        });
        document.getElementById("btn-drawer-close").addEventListener("click", () => this.close());
    }

    toggle() {
        if (this.isOpen)
            this.close();
        else
            this.open(this.activeDrawer);
    }

    open(drawerName = "console") {
        this.activeDrawer = drawerName;
        this.isOpen = true;
        this.drawerEl.classList.remove("hidden");
        this.splitterEl.classList.remove("hidden");
        document.getElementById("btn-drawer-toggle").setAttribute("aria-expanded", "true");
        document.querySelectorAll("#drawer-tabs button").forEach(b => {
            b.setAttribute("aria-selected", b.dataset.drawer === drawerName ? "true" : "false");
        });
        this.renderActive();
    }

    close() {
        this.isOpen = false;
        this.drawerEl.classList.add("hidden");
        this.splitterEl.classList.add("hidden");
        document.getElementById("btn-drawer-toggle").setAttribute("aria-expanded", "false");
    }

    addConsoleEntry(entry) {
        this.consoleEntries.push(entry);
        if (this.consoleEntries.length > 1000)
            this.consoleEntries.shift();
        for (const log of document.querySelectorAll(".console-log"))
            this.renderConsoleMessages(log);
    }

    setEditJournal(journal) {
        this.editJournal = journal || { entries: [], truncated: false };
        if (this.isOpen && this.activeDrawer === "changes")
            this.renderChanges(this.bodyEl);
    }

    renderActive() {
        if (!this.isOpen)
            return;
        switch (this.activeDrawer) {
        case "console":
            this.renderConsole(this.bodyEl);
            break;
        case "changes":
            this.renderChanges(this.bodyEl);
            break;
        case "engine-lens":
            this.renderEngineLens(this.bodyEl);
            break;
        case "quick-source":
            if (this.quickStyleSource)
                this.renderStyleSource(this.quickStyleSource);
            else
                this.app.toolboxPanels.renderSources(this.bodyEl);
            break;
        case "animations":
            this.renderAnimations(this.bodyEl);
            break;
        case "rendering":
            this.renderRendering(this.bodyEl);
            break;
        case "sensors":
            this.renderSensors(this.bodyEl);
            break;
        }
    }

    async openStyleSource(identifier, label, line) {
        const location = { label, line };
        this.quickStyleSource = location;
        this.open("quick-source");
        try {
            const reply = await sendCommand("getStyleSheetSource", identifier);
            location.text = reply.source;
        } catch (err) {
            location.error = err.message;
        }
        if (this.quickStyleSource === location && this.isOpen && this.activeDrawer === "quick-source")
            this.renderStyleSource(location);
    }

    renderStyleSource(location) {
        const heading = el("h3", { class: "section-block", text: location.label });
        const pre = el("pre", { class: "quick-source-code" });
        this.bodyEl.replaceChildren(heading, pre);
        if (location.error || location.text === undefined) {
            pre.textContent = location.error || "Loading stylesheet…";
            return;
        }
        const lines = location.text.split(/\r\n|\r|\n/);
        const index = location.line - 1;
        if (!Number.isInteger(index) || index < 0 || index >= lines.length) {
            pre.textContent = location.text || "(empty stylesheet)";
            return;
        }
        const selected = el("mark", { class: "source-location", text: lines[index] || " " });
        pre.append(lines.slice(0, index).join("\n") + (index ? "\n" : ""), selected,
            index < lines.length - 1 ? "\n" + lines.slice(index + 1).join("\n") : "");
        selected.scrollIntoView({ block: "center" });
    }

    renderConsole(host) {
        host.textContent = "";
        const wrap = el("div", { class: "console-wrap" });

        const filterInput = el("input", {
            type: "search",
            placeholder: "Filter console output",
            value: this.consoleFilter,
            onInput: e => {
                this.consoleFilter = e.target.value;
                this.renderConsoleMessages(logEl);
            },
        });
        const levelSel = el(
            "select",
            { onChange: e => { this.consoleLevel = e.target.value; this.renderConsoleMessages(logEl); } },
            el("option", { value: "all", selected: this.consoleLevel === "all", text: "All levels" }),
            el("option", { value: "error", selected: this.consoleLevel === "error", text: "Errors" }),
            el("option", { value: "warn", selected: this.consoleLevel === "warn", text: "Warnings" }),
            el("option", { value: "info", selected: this.consoleLevel === "info", text: "Info" })
        );
        const preserveCb = el("input", {
            type: "checkbox",
            checked: this.app.state.preserveConsoleLog,
            onChange: () => {
                this.app.state.preserveConsoleLog = preserveCb.checked;
                sendCommand("setPreserveLog", { console: preserveCb.checked }).catch(() => {});
            },
        });
        wrap.appendChild(el(
            "div",
            { class: "pane-toolbar" },
            el("button", {
                class: "chip-btn",
                type: "button",
                text: "Clear",
                onClick: () => {
                    this.consoleEntries = [];
                    sendCommand("clearConsoleLog").catch(() => {});
                    this.renderConsoleMessages(logEl);
                },
            }),
            filterInput,
            levelSel,
            el("label", { class: "inline-check" }, preserveCb, "Preserve log")
        ));

        const logEl = el("div", { class: "console-log" });
        this.renderConsoleMessages(logEl);
        wrap.appendChild(logEl);

        const promptInput = el("input", {
            type: "text",
            placeholder: "Evaluate JavaScript ($0–$4, $_, $(), $$(), $x(), copy(), inspect())",
            "aria-label": "Console expression",
        });
        promptInput.addEventListener("keydown", async e => {
            if (e.key === "Enter" && promptInput.value.trim()) {
                const expr = promptInput.value;
                this.consoleHistory.unshift(expr);
                this.consoleHistoryIdx = -1;
                promptInput.value = "";
                this.addConsoleEntry({ kind: "input", level: "log", text: `› ${expr}` });
                try {
                    const res = await sendCommand("consoleEvaluate", { expression: expr });
                    this.addConsoleEntry({ kind: "result", level: "log", result: res });
                } catch (err) {
                    this.addConsoleEntry({ kind: "error", level: "error", message: err.message });
                }
            } else if (e.key === "ArrowUp") {
                if (this.consoleHistory.length > 0) {
                    e.preventDefault();
                    this.consoleHistoryIdx = Math.min(this.consoleHistory.length - 1, this.consoleHistoryIdx + 1);
                    promptInput.value = this.consoleHistory[this.consoleHistoryIdx];
                }
            } else if (e.key === "ArrowDown") {
                if (this.consoleHistoryIdx > 0) {
                    e.preventDefault();
                    this.consoleHistoryIdx--;
                    promptInput.value = this.consoleHistory[this.consoleHistoryIdx];
                } else {
                    this.consoleHistoryIdx = -1;
                    promptInput.value = "";
                }
            }
        });

        wrap.appendChild(el("div", { class: "console-prompt" }, el("span", { class: "muted monospace", text: "›" }), promptInput));
        host.appendChild(wrap);
    }

    renderConsoleMessages(logEl) {
        logEl.textContent = "";
        const q = this.consoleFilter.trim().toLowerCase();
        for (const entry of this.consoleEntries) {
            if (this.consoleLevel !== "all" && entry.level !== this.consoleLevel)
                continue;
            const formatted = this.formatConsoleEntry(entry);
            if (q && !formatted.toLowerCase().includes(q))
                continue;
            logEl.appendChild(el("div", {
                class: `console-row ${entry.level === "error" ? "error" : entry.level === "warn" ? "warn" : ""}`,
                text: formatted,
            }));
        }
        logEl.scrollTop = logEl.scrollHeight;
    }

    formatConsoleEntry(entry) {
        if (entry.kind === "input")
            return entry.text;
        if (entry.kind === "result") {
            const r = entry.result;
            if (!r)
                return "← undefined";
            if (r.type === "string")
                return `← ${JSON.stringify(r.value)}`;
            if (r.type === "boolean" || r.type === "number")
                return `← ${r.value}`;
            if (r.type === "object")
                return `← ${r.className || "Object"} ${r.text || ""}`;
            return `← ${r.text || r.type}`;
        }
        if (entry.kind === "error")
            return `${entry.name ? `${entry.name}: ` : ""}${entry.message || "Error"}`;
        if (entry.kind === "log" && Array.isArray(entry.arguments))
            return entry.arguments.map(a => (typeof a === "string" ? a : JSON.stringify(a))).join(" ");
        return JSON.stringify(entry);
    }

    // Changes / Flight Recorder
    renderChanges(host) {
        host.textContent = "";
        const entries = this.editJournal.entries || [];
        const copyPatch = async () => {
            const lines = entries.filter(e => !e.undone).map(e => `/* ${e.targetLabel} */\n- ${e.beforeText}\n+ ${e.afterText}`);
            try {
                await sendCommand("copyText", { text: lines.join("\n\n") });
                this.app.showToast("Copied changes");
            } catch (err) {
                this.app.showToast(err.message);
            }
        };

        host.appendChild(el(
            "div",
            { class: "pane-toolbar" },
            el("span", { class: "muted", text: `Flight Recorder (${entries.length} edits${this.editJournal.truncated ? ", oldest evicted" : ""})` }),
            el("button", { class: "chip-btn", type: "button", text: "Copy changes", disabled: entries.length === 0, onClick: copyPatch })
        ));

        if (entries.length === 0) {
            host.appendChild(el("div", { class: "section-block muted", text: "Live DOM and CSS edits in this session will be journaled here." }));
            return;
        }

        // Group by file / node groupKey
        const groups = new Map();
        for (const e of entries) {
            const key = e.groupKey || "live-document";
            if (!groups.has(key))
                groups.set(key, []);
            groups.get(key).push(e);
        }

        for (const [groupKey, list] of groups.entries()) {
            const block = el("div", { class: "section-block" });
            block.appendChild(el(
                "div",
                { style: "display:flex;justify-content:space-between;align-items:center;margin-bottom:6px" },
                el("h3", { class: "monospace", text: `${groupKey} (${list[0].isWritableSource ? "writable source" : "live document"})` })
            ));
            for (const item of list) {
                block.appendChild(el("div", { class: "muted", style: "font-size:11px;margin-top:4px", text: `${item.kind} · ${item.targetLabel}${item.undone ? " (undone)" : ""}` }));
                if (item.canUndo && !item.undone) {
                    block.appendChild(el("button", {
                        class: "chip-btn",
                        type: "button",
                        text: "Undo",
                        "aria-label": `Undo ${item.kind} edit to ${item.targetLabel}`,
                        onClick: async e => {
                            const button = e.currentTarget;
                            button.disabled = true;
                            try {
                                await sendCommand("undoEdit", { id: item.id });
                                await this.app.refreshDomTree();
                                this.app.refreshSelectedNodeProperties();
                            } catch (err) {
                                this.app.showToast(err.message);
                                button.disabled = false;
                            }
                        },
                    }));
                }
                if (item.beforeText)
                    block.appendChild(el("div", { class: "diff-line del", text: `- ${item.beforeText}` }));
                if (item.afterText)
                    block.appendChild(el("div", { class: "diff-line add", text: `+ ${item.afterText}` }));
            }
            host.appendChild(block);
        }
    }

    // Engine Lens (Layout Tree, Paint/DisplayList, Stacking Contexts, Compositor)
    async renderEngineLens(host) {
        host.textContent = "";
        const kinds = [
            ["layout", "Layout Tree"],
            ["paint", "Paint / Display List"],
            ["stacking", "Stacking Contexts"],
            ["accessibility", "Accessibility Dump"],
        ];
        const bar = el("div", { class: "pane-toolbar" });
        for (const [k, label] of kinds) {
            bar.appendChild(el("button", {
                class: `chip-btn ${this.engineLensKind === k ? "active" : ""}`,
                type: "button",
                text: label,
                onClick: () => { this.engineLensKind = k; this.renderEngineLens(host); },
            }));
        }
        bar.appendChild(el("button", {
            class: "chip-btn",
            type: "button",
            text: "Capture",
            onClick: () => this.renderEngineLens(host),
        }));
        host.appendChild(bar);

        const pre = el("pre", {
            class: "monospace",
            style: "padding:10px;overflow:auto;flex:1;font-size:11px;white-space:pre",
            text: "Capturing engine view…",
        });
        host.appendChild(pre);

        try {
            const res = await sendCommand("getEngineLens", { lens: this.engineLensKind });
            pre.textContent = `Generation #${res.documentGeneration} · ${res.lens}\n\n${res.text || "(empty)"}`;
        } catch (err) {
            pre.textContent = `Could not capture Engine Lens: ${err.message}`;
        }
    }

    // Animations Drawer
    async renderAnimations(host) {
        host.textContent = "";
        const bar = el("div", { class: "pane-toolbar" });
        bar.appendChild(el("button", {
            class: "chip-btn",
            type: "button",
            text: "Refresh",
            onClick: () => this.renderAnimations(host),
        }));
        host.appendChild(bar);

        const body = el("div", { class: "section-block muted", text: "Loading active animations…" });
        host.appendChild(body);
        try {
            const res = await sendEngineCommand("getAnimations");
            const list = res?.animations || [];
            body.textContent = "";
            if (list.length === 0) {
                body.textContent = "No animations found in this document.";
                return;
            }
            for (const anim of list) {
                body.appendChild(el(
                    "div",
                    { style: "display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid var(--separator-soft)" },
                    el("button", {
                        class: "source-link",
                        type: "button",
                        text: `${anim.target} · ${anim.name} (${anim.playState})`,
                        onClick: () => {
                            this.app.selectPanel("elements");
                            this.app.elementsTree.selectNode(anim.nodeId, null, { reveal: true, focusRow: true });
                        },
                    }),
                    el("span", { class: "tabular muted", text: anim.duration || "" })
                ));
            }
        } catch (err) {
            body.textContent = err.message;
        }
    }

    // Rendering Emulation Drawer
    renderRendering(host) {
        host.textContent = "";
        const select = el("select", {
            onChange: async () => {
                const scheme = select.value;
                try {
                    await sendCommand("setRenderingOverrides", { prefersColorScheme: scheme });
                    this.colorScheme = scheme;
                } catch (err) {
                    select.value = this.colorScheme;
                    this.app.showToast(err.message);
                }
            },
        });
        for (const [value, text] of [["auto", "System"], ["light", "Light"], ["dark", "Dark"]])
            select.appendChild(el("option", { value, text, selected: this.colorScheme === value }));
        host.appendChild(el("label", { class: "inline-field" }, "Color scheme", select));
    }

    // Sensors Drawer
    renderSensors(host) {
        host.textContent = "";
        const field = (label, key, placeholder) => {
            const inp = el("input", {
                type: "text",
                value: this.sensorState[key],
                placeholder,
                onInput: e => { this.sensorState[key] = e.target.value; },
            });
            return el("label", { style: "display:flex;align-items:center;justify-content:space-between;gap:12px;padding:6px 12px" }, el("span", { text: label }), inp);
        };
        host.appendChild(field("Geolocation latitude", "latitude", "e.g. 37.7749"));
        host.appendChild(field("Geolocation longitude", "longitude", "e.g. -122.4194"));
        host.appendChild(el(
            "div",
            { style: "padding:8px 12px;display:flex;gap:8px" },
            el("button", {
                class: "chip-btn primary",
                type: "button",
                text: "Apply location",
                onClick: async () => {
                    const { latitude, longitude } = this.sensorState;
                    if (!latitude.trim() || !longitude.trim() || !Number.isFinite(Number(latitude)) || !Number.isFinite(Number(longitude))
                        || Math.abs(Number(latitude)) > 90 || Math.abs(Number(longitude)) > 180) {
                        this.app.showToast("Enter a latitude from −90 to 90 and a longitude from −180 to 180.");
                        return;
                    }
                    try {
                        await sendEngineCommand("setSensorOverrides", { latitude: Number(latitude), longitude: Number(longitude) });
                        this.app.showToast("Location applied");
                    } catch (err) {
                        this.app.showToast(err.message);
                    }
                },
            }),
            el("button", {
                class: "chip-btn",
                type: "button",
                text: "Reset",
                onClick: async () => {
                    try {
                        await sendEngineCommand("setSensorOverrides", { enabled: false });
                        this.sensorState = { latitude: "", longitude: "" };
                        this.renderSensors(host);
                    } catch (err) {
                        this.app.showToast(err.message);
                    }
                },
            })
        ));
    }
}
