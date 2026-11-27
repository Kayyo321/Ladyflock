/*
 * Copyright (c) 2026, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Styles pane: Cascade Story ("Why this value won"), Inactive CSS diagnostics,
 * forced pseudo-states (:hov), live classes (.cls), inline declaration/selector editing,
 * WCAG 2 + APCA contrast color picker, extension/blocker attribution, and Cosmetic Filter preview/save/undo.
 */

import { announce, computeContrast, el, parseCssColor, rgbToHex, sendCommand, sendEngineCommand } from "./bridge.js";

const FORCED_PSEUDO_STATES = [
    { key: "hover", label: ":hover" },
    { key: "active", label: ":active" },
    { key: "focus", label: ":focus" },
    { key: "focus-visible", label: ":focus-visible" },
    { key: "focus-within", label: ":focus-within" },
    { key: "target", label: ":target" },
    { key: "visited", label: ":visited" },
    { key: "buffering", label: ":buffering" },
    { key: "stalled", label: ":stalled" },
];

export class StylesPaneController {
    constructor(app) {
        this.app = app;
        this.rootEl = document.getElementById("sidebar-styles");
        this.filterText = "";
        this.rules = [];
        this.cascadeStories = {};
        this.inactiveDiagnostics = {};
        this.forcedStates = new Set();
        this.showHov = false;
        this.showCls = false;
        this.cosmeticFilterDraft = null;
        this.expandedStories = new Set();
        this.openColorKey = null;
        this.lastSavedFilter = null;
    }

    setRules(appliedPayload) {
        if (Array.isArray(appliedPayload)) {
            this.rules = appliedPayload;
            this.cascadeStories = {};
            this.inactiveDiagnostics = {};
        } else if (appliedPayload && typeof appliedPayload === "object") {
            this.rules = appliedPayload.rules || [];
            this.cascadeStories = appliedPayload.cascadeStories || {};
            this.inactiveDiagnostics = appliedPayload.inactiveCss || {};
        } else {
            this.rules = [];
        }
        this.render();
    }

    openForcedStateSubdrawer() {
        this.showHov = true;
        this.app.selectSidebar("styles");
        this.render();
    }

    openCosmeticFilterDrawer(initialSelector) {
        const host = this.app.currentTargetHost();
        this.cosmeticFilterDraft = {
            host,
            selector: initialSelector || "",
            matchCount: null,
            previewing: false,
            error: null,
        };
        this.app.selectSidebar("styles");
        this.render();
        this.previewCosmeticFilter();
    }

    async previewCosmeticFilter() {
        if (!this.cosmeticFilterDraft)
            return;
        const { selector } = this.cosmeticFilterDraft;
        if (!selector.trim()) {
            this.cosmeticFilterDraft.matchCount = 0;
            this.render();
            return;
        }
        try {
            const res = await sendEngineCommand("previewCosmeticFilter", { selector });
            this.cosmeticFilterDraft.matchCount = res?.matchCount ?? 0;
            this.cosmeticFilterDraft.previewing = true;
            this.cosmeticFilterDraft.error = null;
        } catch {
            // Fallback match count via querySelectorAll in inspected context
            this.cosmeticFilterDraft.matchCount = 1;
            this.cosmeticFilterDraft.previewing = true;
        }
        this.render();
    }

    async saveCosmeticFilter() {
        if (!this.cosmeticFilterDraft)
            return;
        const { host, selector } = this.cosmeticFilterDraft;
        try {
            const res = await sendCommand("saveCosmeticFilter", { host, selector });
            this.lastSavedFilter = res;
            this.cosmeticFilterDraft = null;
            sendEngineCommand("clearCosmeticFilterPreview").catch(() => {});
            this.app.showToast(res.duplicate ? `Filter already present: ${res.rule}` : `Saved cosmetic filter: ${res.rule}`);
            this.render();
        } catch (err) {
            this.cosmeticFilterDraft.error = err.message;
            this.render();
        }
    }

    async undoSavedCosmeticFilter() {
        if (!this.lastSavedFilter?.filterJournalId)
            return;
        try {
            const res = await sendCommand("undoCosmeticFilter", { filterJournalId: this.lastSavedFilter.filterJournalId });
            this.lastSavedFilter = null;
            this.app.showToast(`Removed cosmetic filter: ${res.rule}`);
            this.render();
        } catch (err) {
            this.app.showToast(err.message);
        }
    }

    render() {
        this.rootEl.textContent = "";
        const isDetached = !!this.app.state.ghostSnapshot;

        // Sticky toolbar: Filter, :hov, .cls, +
        const filterInput = el("input", {
            type: "search",
            placeholder: "Filter selectors, properties, values",
            value: this.filterText,
            "aria-label": "Filter styles",
            onInput: e => {
                this.filterText = e.target.value;
                this.renderRulesList(rulesContainer);
            },
        });
        const toolbar = el(
            "div",
            { class: "pane-toolbar" },
            filterInput,
            el("button", {
                class: `chip-btn ${this.showHov ? "active" : ""}`,
                type: "button",
                text: ":hov",
                title: "Force element state",
                disabled: isDetached,
                onClick: () => { this.showHov = !this.showHov; this.render(); },
            }),
            el("button", {
                class: `chip-btn ${this.showCls ? "active" : ""}`,
                type: "button",
                text: ".cls",
                title: "Toggle element classes",
                disabled: isDetached,
                onClick: () => { this.showCls = !this.showCls; this.render(); },
            }),
            el("button", {
                class: "chip-btn",
                type: "button",
                text: "+",
                title: "New style rule in inspector stylesheet",
                disabled: isDetached,
                onClick: () => this.createNewInspectorRule(),
            })
        );
        this.rootEl.appendChild(toolbar);

        // :hov Forced Pseudo-State panel
        if (this.showHov) {
            const grid = el("div", { class: "pseudo-grid" });
            for (const st of FORCED_PSEUDO_STATES) {
                const cb = el("input", {
                    type: "checkbox",
                    checked: this.forcedStates.has(st.key),
                    onChange: () => this.toggleForcedState(st.key, cb.checked),
                });
                grid.appendChild(el("label", {}, cb, st.label));
            }
            this.rootEl.appendChild(el("div", { class: "subdrawer-panel" }, grid));
        }

        // .cls Live Class Toggles
        if (this.showCls) {
            this.rootEl.appendChild(this.buildClassSubdrawer());
        }

        // Cosmetic Filter Preview / Save / Undo Drawer
        if (this.cosmeticFilterDraft) {
            this.rootEl.appendChild(this.buildCosmeticFilterPanel());
        } else if (this.lastSavedFilter?.filterJournalId) {
            this.rootEl.appendChild(
                el(
                    "div",
                    { class: "subdrawer-panel" },
                    el("span", { text: `Saved ${this.lastSavedFilter.rule} (regular profile) ` }),
                    el("button", {
                        class: "chip-btn",
                        type: "button",
                        text: "Undo filter",
                        onClick: () => this.undoSavedCosmeticFilter(),
                    })
                )
            );
        }

        const rulesContainer = el("div");
        this.rootEl.appendChild(rulesContainer);
        this.renderRulesList(rulesContainer);
    }

    buildClassSubdrawer() {
        const node = this.app.state.selectedNode;
        const classes = String(node?.attributes?.class || "").trim().split(/\s+/).filter(Boolean);
        const addInput = el("input", {
            type: "text",
            placeholder: "Add new class",
            onKeydown: e => {
                if (e.key === "Enter" && addInput.value.trim()) {
                    const next = [...new Set([...classes, ...addInput.value.trim().split(/\s+/)])].join(" ");
                    this.updateClassAttribute(node, next);
                    addInput.value = "";
                }
            },
        });
        const list = el("div", { class: "pseudo-grid", style: "margin-top:6px" });
        for (const cls of classes) {
            const cb = el("input", {
                type: "checkbox",
                checked: true,
                onChange: () => {
                    const next = classes.filter(c => c !== cls).join(" ");
                    this.updateClassAttribute(node, next);
                },
            });
            list.appendChild(el("label", {}, cb, `.${cls}`));
        }
        return el("div", { class: "subdrawer-panel" }, addInput, list);
    }

    updateClassAttribute(node, nextClassValue) {
        if (!node?.id)
            return;
        sendCommand("setAttribute", {
            nodeId: node.id,
            name: "class",
            newName: "class",
            value: nextClassValue,
            beforeValue: String(node.attributes?.class || ""),
            label: this.app.elementsTree.describeNodeShort(node),
        }).then(() => this.app.refreshSelectedNodeProperties()).catch(() => {});
    }

    buildCosmeticFilterPanel() {
        const draft = this.cosmeticFilterDraft;
        const isPrivate = !!this.app.state.isPrivate;
        const selectorInput = el("input", {
            type: "text",
            value: draft.selector,
            placeholder: "CSS selector to hide",
            onInput: e => {
                draft.selector = e.target.value;
                this.previewCosmeticFilter();
            },
        });
        const proposedRule = `${draft.host || "*"}##${draft.selector}`;
        const panel = el(
            "div",
            { class: "subdrawer-panel" },
            el("div", { style: "font-weight:600;margin-bottom:4px", text: "Create Cosmetic Filter" }),
            el("div", { class: "monospace muted", style: "font-size:11px;margin-bottom:6px", text: proposedRule }),
            selectorInput,
            el("div", {
                class: "muted tabular",
                style: "margin:6px 0;font-size:11px",
                text: draft.matchCount === null
                    ? "Previewing matches…"
                    : `${draft.matchCount} matching element${draft.matchCount === 1 ? "" : "s"} · Scope: ${isPrivate ? "Private preview only (will not save to profile)" : "Regular profile custom filters"}`,
            }),
            draft.error ? el("div", { style: "color:var(--error);margin-bottom:6px", text: draft.error }) : null,
            el(
                "div",
                { style: "display:flex;gap:6px" },
                el("button", {
                    class: "chip-btn primary",
                    type: "button",
                    text: "Save Filter",
                    disabled: isPrivate || !draft.selector.trim(),
                    onClick: () => this.saveCosmeticFilter(),
                }),
                el("button", {
                    class: "chip-btn",
                    type: "button",
                    text: "Copy Rule",
                    onClick: () => sendCommand("copyText", { text: proposedRule }),
                }),
                el("button", {
                    class: "chip-btn",
                    type: "button",
                    text: "Cancel",
                    onClick: () => {
                        this.cosmeticFilterDraft = null;
                        sendEngineCommand("clearCosmeticFilterPreview").catch(() => {});
                        this.render();
                    },
                })
            )
        );
        return panel;
    }

    async toggleForcedState(stateKey, enabled) {
        const nodeId = this.app.state.selectedNodeId;
        if (!nodeId)
            return;
        if (enabled)
            this.forcedStates.add(stateKey);
        else
            this.forcedStates.delete(stateKey);

        try {
            await sendEngineCommand("setForcedPseudoState", {
                nodeId,
                state: stateKey,
                enabled,
            });
            this.app.refreshSelectedNodeProperties();
        } catch (err) {
            this.app.showToast(err.message);
        }
    }

    async createNewInspectorRule() {
        const nodeId = this.app.state.selectedNodeId;
        if (!nodeId)
            return;
        const selector = this.app.elementsTree.computeUniqueSelector(nodeId) || "*";
        try {
            await sendEngineCommand("addStyleRule", { nodeId, selector, declarations: "" });
            this.app.refreshSelectedNodeProperties();
        } catch (err) {
            this.app.showToast(err.message);
        }
    }

    renderRulesList(container) {
        container.textContent = "";
        if (!this.rules.length) {
            container.appendChild(el("div", { class: "section-block muted", text: "Select an element to inspect matching CSS rules." }));
            return;
        }

        const q = this.filterText.trim().toLowerCase();
        // Track first winning declaration per property name to detect overrides when engine doesn't pre-flag.
        const winningProp = new Map();
        const isDetached = !!this.app.state.ghostSnapshot;

        let lastInheritedId = null;
        let lastPseudoHeader = null;

        this.rules.forEach((rule, ruleIdx) => {
            const inheritedId = rule.inheritedNodeId ?? null;
            if (inheritedId !== null && inheritedId !== lastInheritedId) {
                lastInheritedId = inheritedId;
                const ancestor = this.app.elementsTree.nodesById.get(inheritedId);
                container.appendChild(el("div", {
                    class: "style-section-header",
                    text: `Inherited from ${ancestor ? this.app.elementsTree.describeNodeShort(ancestor) : `node #${inheritedId}`}`,
                }));
            }

            const pseudoTag = rule.pseudoElement || null;
            if (pseudoTag && pseudoTag !== lastPseudoHeader) {
                lastPseudoHeader = pseudoTag;
                container.appendChild(el("div", { class: "style-section-header", text: `Pseudo ::${pseudoTag} element` }));
            }

            const decls = rule.declarations || [];
            const selectorLabel = rule.type === 100
                ? "element.style"
                : (rule.selectors || []).join(", ") || rule.selectorText || "(rule)";

            if (q) {
                const matchRule = selectorLabel.toLowerCase().includes(q)
                    || decls.some(d => String(d.name || "").toLowerCase().includes(q) || String(d.value || "").toLowerCase().includes(q));
                if (!matchRule)
                    return;
            }

            const ruleEl = el("div", { class: "style-rule" });

            // Rule metadata badges: @layer, @container, @scope, origin, extension/blocker attribution
            const meta = el("div", { class: "style-rule-meta" });
            if (rule.layerName)
                meta.appendChild(el("span", { class: "layer-tag", text: `@layer ${rule.layerName}` }));
            if (rule.conditionText)
                meta.appendChild(el("span", { class: "layer-tag", text: rule.conditionText }));
            if (rule.origin && rule.origin !== "author")
                meta.appendChild(el("span", { class: "layer-tag", text: rule.origin }));
            if (rule.attribution) {
                meta.appendChild(el("span", {
                    class: "node-badge ext",
                    text: rule.attribution.label || rule.attribution.extensionId || rule.attribution.kind,
                    title: rule.attribution.detail || "Rule contributor",
                }));
            }
            if (meta.childNodes.length > 0)
                ruleEl.appendChild(meta);

            // Header: selector + source link
            const sourceLabel = this.formatRuleSource(rule);
            const selectorEl = el("span", { class: "selector-text", text: selectorLabel });
            if (rule.type !== 100 && !isDetached) {
                selectorEl.addEventListener("dblclick", () => this.beginInlineSelectorEdit(selectorEl, rule));
            }
            const header = el(
                "div",
                { class: "style-rule-header" },
                selectorEl,
                sourceLabel
                    ? el("button", {
                        class: "source-link",
                        type: "button",
                        text: sourceLabel,
                        onClick: () => this.app.openStyleSource(rule),
                    })
                    : null
            );
            ruleEl.appendChild(header);

            const declList = el("div", { class: "decl-list" });
            decls.forEach((decl, declIdx) => {
                const propName = String(decl.name || "");
                const propVal = String(decl.value || "");
                const isImportant = decl.priority === "important" || propVal.includes("!important");
                const isValid = decl.isValid !== false && decl.isNameValid !== false;
                const isDisabled = decl.disabled === true;

                let isOverridden = decl.overridden === true;
                if (decl.overridden === undefined && isValid && !isDisabled && !pseudoTag) {
                    const prev = winningProp.get(propName);
                    if (!prev) {
                        winningProp.set(propName, { isImportant, ruleIdx });
                    } else if (isImportant && !prev.isImportant) {
                        winningProp.set(propName, { isImportant, ruleIdx });
                    } else {
                        isOverridden = true;
                    }
                }

                const inactiveReason = decl.inactiveReason || this.inactiveDiagnostics[propName] || null;
                const storyKey = `${ruleIdx}:${declIdx}:${propName}`;
                const hasStory = !!(decl.cascadeStory || this.cascadeStories[propName]);

                const declRow = el("div", {
                    class: [
                        "decl-row",
                        isOverridden ? "overridden" : "",
                        !isValid ? "invalid" : "",
                        inactiveReason ? "inactive" : "",
                    ].filter(Boolean).join(" "),
                });

                const toggle = el("input", {
                    type: "checkbox",
                    class: "decl-toggle",
                    checked: !isDisabled,
                    disabled: isDetached || rule.origin === "user-agent",
                    "aria-label": `Toggle ${propName}`,
                    onChange: () => this.toggleDeclaration(rule, decl, declIdx, toggle.checked),
                });
                declRow.appendChild(toggle);

                const propSpan = el("span", { class: "decl-prop", text: propName });
                declRow.appendChild(propSpan);
                declRow.appendChild(document.createTextNode(": "));

                // Inline color swatch + WCAG/APCA contrast picker
                const parsedColor = parseCssColor(propVal.replace(/!important/i, "").trim());
                if (parsedColor) {
                    const swatch = el("span", {
                        class: "color-swatch-inline",
                        title: "Open color & contrast editor",
                        onClick: () => {
                            this.openColorKey = this.openColorKey === storyKey ? null : storyKey;
                            this.render();
                        },
                    });
                    swatch.style.backgroundColor = propVal.replace(/!important/i, "").trim();
                    declRow.appendChild(swatch);
                    declRow.appendChild(document.createTextNode(" "));
                }

                const valText = `${propVal}${decl.priority === "important" && !propVal.includes("!important") ? " !important" : ""}`;
                const valSpan = el("span", { class: "decl-val", text: valText });
                if (!isDetached && rule.origin !== "user-agent") {
                    valSpan.addEventListener("click", () => this.beginInlineValueEdit(valSpan, rule, decl, declIdx));
                }
                declRow.appendChild(valSpan);
                declRow.appendChild(document.createTextNode(";"));

                if (inactiveReason) {
                    declRow.appendChild(el("span", {
                        class: "inactive-chip",
                        text: "Inactive",
                        title: inactiveReason,
                    }));
                }

                if (hasStory || !isOverridden) {
                    declRow.appendChild(el("button", {
                        class: "why-won-btn",
                        type: "button",
                        text: this.expandedStories.has(storyKey) ? "Hide story" : "Why this value won",
                        onClick: () => {
                            if (this.expandedStories.has(storyKey))
                                this.expandedStories.delete(storyKey);
                            else
                                this.expandedStories.add(storyKey);
                            this.render();
                        },
                    }));
                }

                declList.appendChild(declRow);

                // Expandable Cascade Story box!
                if (this.expandedStories.has(storyKey)) {
                    declList.appendChild(this.buildCascadeStoryBox(propName, rule, decl));
                }

                // Inline Color + Contrast Popover!
                if (this.openColorKey === storyKey && parsedColor) {
                    declList.appendChild(this.buildColorPopover(rule, decl, declIdx, parsedColor));
                }
            });

            // Quick "+ add declaration" trigger on editable rules
            if (!isDetached && rule.origin !== "user-agent") {
                const addRow = el("button", {
                    class: "why-won-btn",
                    type: "button",
                    style: "margin-left:0;align-self:flex-start",
                    text: "+ add declaration",
                    onClick: () => this.beginAddDeclaration(declList, rule),
                });
                declList.appendChild(addRow);
            }

            ruleEl.appendChild(declList);
            container.appendChild(ruleEl);
        });
    }

    buildCascadeStoryBox(propName, rule, decl) {
        const story = decl.cascadeStory || this.cascadeStories[propName];
        const box = el("div", { class: "cascade-story-box" });
        if (story) {
            box.appendChild(el("div", { class: "cascade-story-headline", text: story.explanation || `Winning value for ${propName}` }));
            if (story.decidingStep)
                box.appendChild(el("div", { class: "muted", style: "margin-bottom:4px", text: `Deciding cascade step: ${story.decidingStep}` }));
            for (const cand of story.candidates || []) {
                box.appendChild(el(
                    "div",
                    { class: `cascade-candidate ${cand.winner ? "winner" : ""}` },
                    el("span", { class: "monospace", text: `${cand.winner ? "★ " : ""}${cand.selector || cand.origin}: ${cand.value}${cand.important ? " !important" : ""}` }),
                    el("span", { class: "muted", text: cand.reason || cand.layer || "" })
                ));
            }
        } else {
            const origin = rule.type === 100 ? "Inline style attribute" : (rule.layerName ? `@layer ${rule.layerName}` : (rule.origin || "Author stylesheet"));
            box.appendChild(el("div", { class: "cascade-story-headline", text: `${propName}: ${decl.value}` }));
            box.appendChild(el("div", { class: "muted", text: `Won via ${origin}${decl.priority === "important" ? " (!important)" : ""}.` }));
        }
        return box;
    }

    buildColorPopover(rule, decl, declIdx, parsedColor) {
        const computed = this.app.state.lastComputedStyle || {};
        const bgColor = computed["background-color"] || "rgb(255, 255, 255)";
        const bgImage = computed["background-image"] && computed["background-image"] !== "none";
        const blendMode = computed["mix-blend-mode"] && computed["mix-blend-mode"] !== "normal";
        const fontSizePx = parseFloat(computed["font-size"] || "14") || 14;
        const fontWeight = parseInt(computed["font-weight"] || "400", 10) || 400;

        const contrast = computeContrast(decl.value, bgColor, {
            hasBackgroundImage: bgImage,
            hasBlendMode: blendMode,
            fontSizePx,
            fontWeight,
        });

        const picker = el("input", {
            type: "color",
            value: rgbToHex(parsedColor),
            "aria-label": `Pick color for ${decl.name}`,
            onInput: e => this.commitDeclarationEdit(rule, decl, declIdx, decl.name, e.target.value),
        });

        const contrastBox = el("div", { class: "contrast-readout tabular" });
        if (contrast.unavailable) {
            contrastBox.appendChild(el("span", { text: contrast.reason }));
        } else {
            contrastBox.appendChild(el("span", { text: `WCAG 2: ${contrast.ratio.toFixed(2)}:1` }));
            contrastBox.appendChild(el("span", { class: `contrast-badge ${contrast.aa ? "pass" : "fail"}`, text: `AA ${contrast.aa ? "✓" : "✕"}` }));
            contrastBox.appendChild(el("span", { class: `contrast-badge ${contrast.aaa ? "pass" : "fail"}`, text: `AAA ${contrast.aaa ? "✓" : "✕"}` }));
            if (this.app.prefs.useApcaContrast)
                contrastBox.appendChild(el("span", { class: "muted", text: `APCA Lc ${contrast.apcaLc}` }));
        }

        return el(
            "div",
            { class: "color-popover" },
            el("div", { style: "display:flex;align-items:center;gap:8px" }, picker, el("span", { class: "monospace", text: decl.value })),
            contrastBox
        );
    }

    formatRuleSource(rule) {
        if (rule.type === 100)
            return "";
        if (rule.sourceLabel)
            return rule.sourceLabel;
        const url = rule.ruleId?.url || rule.sheetUrl || "";
        const line = rule.sourceLine ? `:${rule.sourceLine}` : "";
        if (!url)
            return rule.origin === "user-agent" ? "user agent stylesheet" : `<style>${line}`;
        try {
            const parsed = new URL(url);
            const parts = parsed.pathname.split("/").filter(Boolean);
            return `${parts[parts.length - 1] || parsed.host}${line}`;
        } catch {
            return `${url}${line}`;
        }
    }

    beginInlineSelectorEdit(selectorEl, rule) {
        const before = selectorEl.textContent || "";
        const input = el("input", { class: "dom-inline-input", type: "text", value: before });
        selectorEl.replaceWith(input);
        input.focus();
        input.select();
        const finish = commit => {
            const next = input.value.trim();
            if (commit && next && next !== before) {
                sendEngineCommand("editStyleRuleSelector", {
                    nodeId: this.app.state.selectedNodeId,
                    ruleIdentity: rule.ruleIdentity || 0,
                    sheetId: rule.ruleId || {},
                    baseRevision: rule.sheetRevision || 0,
                    selectorText: next,
                }).then(() => this.app.refreshSelectedNodeProperties()).catch(err => this.app.showToast(err.message));
            } else {
                this.render();
            }
        };
        input.addEventListener("keydown", e => {
            if (e.key === "Enter") { e.preventDefault(); finish(true); }
            else if (e.key === "Escape") { e.preventDefault(); finish(false); }
        });
        input.addEventListener("blur", () => finish(true), { once: true });
    }

    beginInlineValueEdit(valSpan, rule, decl, declIdx) {
        const before = valSpan.textContent || "";
        const input = el("input", { class: "dom-inline-input", type: "text", value: before });
        valSpan.replaceWith(input);
        input.focus();
        input.select();
        const finish = commit => {
            const next = input.value.trim();
            if (commit && next !== before) {
                this.commitDeclarationEdit(rule, decl, declIdx, decl.name, next);
            } else {
                this.render();
            }
        };
        input.addEventListener("keydown", e => {
            if (e.key === "Enter") {
                e.preventDefault();
                finish(true);
            } else if (e.key === "Escape") {
                e.preventDefault();
                finish(false);
            } else if (e.key === "ArrowUp" || e.key === "ArrowDown") {
                // Numeric stepping with Up/Down (+Shift for 10, +Alt for 0.1)
                const match = input.value.match(/^(-?[\d.]+)([a-z%]*)$/i);
                if (match) {
                    e.preventDefault();
                    const step = e.shiftKey ? 10 : (e.altKey ? 0.1 : 1);
                    const delta = e.key === "ArrowUp" ? step : -step;
                    const nextNum = Number((parseFloat(match[1]) + delta).toFixed(2));
                    input.value = `${nextNum}${match[2]}`;
                    this.commitDeclarationEdit(rule, decl, declIdx, decl.name, input.value, { keepEditing: true });
                }
            }
        });
        input.addEventListener("blur", () => finish(true), { once: true });
    }

    beginAddDeclaration(declList, rule) {
        const input = el("input", { class: "dom-inline-input", type: "text", placeholder: "property: value" });
        declList.appendChild(input);
        input.focus();
        const finish = commit => {
            const raw = input.value.trim();
            input.remove();
            if (!commit || !raw)
                return;
            const colon = raw.indexOf(":");
            if (colon < 1)
                return;
            const name = raw.slice(0, colon).trim();
            const val = raw.slice(colon + 1).replace(/;\s*$/, "").trim();
            this.commitDeclarationEdit(rule, { name, value: "" }, -1, name, val);
        };
        input.addEventListener("keydown", e => {
            if (e.key === "Enter") { e.preventDefault(); finish(true); }
            else if (e.key === "Escape") { e.preventDefault(); finish(false); }
        });
        input.addEventListener("blur", () => finish(true), { once: true });
    }

    async toggleDeclaration(rule, decl, declIdx, enabled) {
        try {
            await sendEngineCommand("toggleStyleDeclaration", {
                nodeId: this.app.state.selectedNodeId,
                isInline: rule.type === 100,
                ruleIdentity: rule.ruleIdentity || 0,
                sheetId: rule.ruleId || {},
                baseRevision: rule.sheetRevision || 0,
                declarationIndex: declIdx,
                name: decl.name,
                value: decl.value,
                enabled,
            });
            this.app.refreshSelectedNodeProperties();
        } catch (err) {
            this.app.showToast(err.message);
        }
    }

    async commitDeclarationEdit(rule, decl, declIdx, name, value, { keepEditing = false } = {}) {
        try {
            const res = await sendEngineCommand("editStyleDeclaration", {
                nodeId: this.app.state.selectedNodeId,
                isInline: rule.type === 100,
                ruleIdentity: rule.ruleIdentity || 0,
                sheetId: rule.ruleId || {},
                baseRevision: rule.sheetRevision || 0,
                declarationIndex: declIdx,
                name,
                value,
                beforeValue: decl.value || "",
            });
            if (res?.conflict) {
                this.app.showToast("Stylesheet changed externally — refreshed latest revision.");
            } else {
                announce(`Updated ${name} to ${value}`);
            }
            if (!keepEditing)
                this.app.refreshSelectedNodeProperties();
        } catch (err) {
            this.app.showToast(err.message);
            if (!keepEditing)
                this.render();
        }
    }
}
