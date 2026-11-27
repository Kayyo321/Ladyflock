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
        this.animations = [];
        this.renderingState = {
            mediaType: "screen",
            prefersColorScheme: "auto",
            prefersReducedMotion: "no-preference",
            prefersContrast: "no-preference",
            visionDeficiency: "none",
            paintFlashing: false,
            layerBorders: false,
            layoutShiftRegions: false,
        };
        this.sensorState = {
            latitude: "",
            longitude: "",
            locale: "",
            timezone: "",
        };

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
        if (this.isOpen && this.activeDrawer === "console")
            this.renderConsole(this.bodyEl);
        if (this.app.state.activePanel === "console")
            this.renderConsole(document.getElementById("panel-console"));
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
        const copyPatch = () => {
            const lines = entries.filter(e => !e.undone).map(e => `/* ${e.targetLabel} */\n- ${e.beforeText}\n+ ${e.afterText}`);
            sendCommand("copyText", { text: lines.join("\n\n") });
            this.app.showToast("Copied patch to clipboard");
        };

        host.appendChild(el(
            "div",
            { class: "pane-toolbar" },
            el("span", { class: "muted", text: `Flight Recorder (${entries.length} edits${this.editJournal.truncated ? ", oldest evicted" : ""})` }),
            el("button", { class: "chip-btn", type: "button", text: "Copy Patch", disabled: entries.length === 0, onClick: copyPatch })
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
        const rates = [1, 0.25, 0.1];
        const bar = el("div", { class: "pane-toolbar" }, el("span", { class: "muted", text: "Playback speed:" }));
        for (const rate of rates) {
            bar.appendChild(el("button", {
                class: "chip-btn tabular",
                type: "button",
                text: `${Math.round(rate * 100)}%`,
                onClick: () => sendEngineCommand("setAnimationsPlaybackRate", { rate }).catch(() => {}),
            }));
        }
        bar.appendChild(el("button", {
            class: "chip-btn",
            type: "button",
            text: "Pause all",
            onClick: () => sendEngineCommand("pauseAllAnimations", { paused: true }).catch(() => {}),
        }));
        bar.appendChild(el("button", {
            class: "chip-btn",
            type: "button",
            text: "Resume all",
            onClick: () => sendEngineCommand("pauseAllAnimations", { paused: false }).catch(() => {}),
        }));
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
                body.textContent = "No active CSS animations, transitions, or Web Animations in this document.";
                return;
            }
            for (const anim of list) {
                body.appendChild(el(
                    "div",
                    { style: "display:flex;justify-content:space-between;padding:4px 0;border-bottom:1px solid var(--separator-soft)" },
                    el("span", { class: "monospace", text: `${anim.name || "animation"} (${anim.playState})` }),
                    el("span", { class: "tabular muted", text: `${Math.round(anim.currentTime || 0)}ms / ${anim.duration || "auto"}` })
                ));
            }
        } catch {
            body.textContent = "Animations timeline ready — trigger an animation on the page and click Refresh.";
        }
    }

    // Rendering Emulation Drawer
    renderRendering(host) {
        host.textContent = "";
        const apply = () => {
            sendCommand("setRenderingOverrides", this.renderingState).catch(() => {});
            sendEngineCommand("setRenderingOverrides", this.renderingState).catch(() => {});
        };

        const row = (label, control) => el("div", { style: "display:flex;align-items:center;justify-content:space-between;padding:6px 12px;border-bottom:1px solid var(--separator-soft)" }, el("span", { text: label }), control);

        const makeSelect = (key, options) => {
            const sel = el("select", {
                onChange: () => { this.renderingState[key] = sel.value; apply(); },
            });
            for (const [val, txt] of options)
                sel.appendChild(el("option", { value: val, selected: this.renderingState[key] === val, text: txt }));
            return sel;
        };

        const makeCheck = key => {
            const cb = el("input", {
                type: "checkbox",
                checked: !!this.renderingState[key],
                onChange: () => { this.renderingState[key] = cb.checked; apply(); },
            });
            return cb;
        };

        host.appendChild(row("Emulate prefers-color-scheme", makeSelect("prefersColorScheme", [["auto", "No override"], ["light", "Light"], ["dark", "Dark"]])));
        host.appendChild(row("Emulate CSS media type", makeSelect("mediaType", [["screen", "screen"], ["print", "print"]])));
        host.appendChild(row("Emulate prefers-reduced-motion", makeSelect("prefersReducedMotion", [["no-preference", "No override"], ["reduce", "reduce"]])));
        host.appendChild(row("Emulate prefers-contrast", makeSelect("prefersContrast", [["no-preference", "No override"], ["more", "more"], ["less", "less"]])));
        host.appendChild(row("Emulate vision deficiency", makeSelect("visionDeficiency", [["none", "None"], ["blurred", "Blurred vision"], ["protanopia", "Protanopia"], ["deuteranopia", "Deuteranopia"], ["tritanopia", "Tritanopia"], ["achromatopsia", "Achromatopsia"]])));
        host.appendChild(row("Paint flashing", makeCheck("paintFlashing")));
        host.appendChild(row("Layer borders", makeCheck("layerBorders")));
        host.appendChild(row("Layout shift regions", makeCheck("layoutShiftRegions")));
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
            return el("div", { style: "display:flex;align-items:center;justify-content:space-between;gap:12px;padding:6px 12px" }, el("span", { text: label }), inp);
        };
        host.appendChild(field("Geolocation latitude", "latitude", "e.g. 37.7749"));
        host.appendChild(field("Geolocation longitude", "longitude", "e.g. -122.4194"));
        host.appendChild(field("Locale override", "locale", "e.g. en-US, ja-JP"));
        host.appendChild(field("Timezone ID", "timezone", "e.g. America/New_York"));
        host.appendChild(el(
            "div",
            { style: "padding:8px 12px;display:flex;gap:8px" },
            el("button", {
                class: "chip-btn primary",
                type: "button",
                text: "Apply Sensors",
                onClick: () => {
                    sendEngineCommand("setSensorOverrides", this.sensorState).catch(() => {});
                    this.app.showToast("Applied sensor overrides");
                },
            }),
            el("button", {
                class: "chip-btn",
                type: "button",
                text: "Reset",
                onClick: () => {
                    this.sensorState = { latitude: "", longitude: "", locale: "", timezone: "" };
                    sendEngineCommand("setSensorOverrides", this.sensorState).catch(() => {});
                    this.renderSensors(host);
                },
            })
        ));
    }
}
