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
        this.forcedStates = new Map();
        this.showHov = false;
        this.showCls = false;
        this.cosmeticFilterDraft = null;
        this.expandedStories = new Set();
        this.openColorKey = null;
        this.lastSavedFilter = null;
    }

    setRules(appliedPayload) {
        if (Array.isArray(appliedPayload)) {
            this.rules = appliedPayload.map(({ rule, inheritedNodeId }) => ({ ...rule, inheritedNodeId }));
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
        const draft = this.cosmeticFilterDraft;
        const { selector } = draft;
        draft.previewing = false;
        draft.matchCount = null;
        draft.error = null;
        try {
            if (!selector.trim())
                throw new Error("Enter a CSS selector.");
            const res = await sendEngineCommand("previewCosmeticFilter", { selector });
            if (this.cosmeticFilterDraft !== draft || selector !== draft.selector)
                return;
            if (!res.valid)
                throw new Error(res.error || "Invalid CSS selector.");
            draft.matchCount = res.count;
            draft.previewing = true;
        } catch (err) {
            if (this.cosmeticFilterDraft !== draft || selector !== draft.selector)
                return;
            draft.error = err.message;
        }
        const restoreFocus = document.activeElement?.id === "cosmetic-filter-selector";
        const caret = restoreFocus ? document.activeElement.selectionStart : null;
        this.render();
        if (restoreFocus) {
            const input = document.getElementById("cosmetic-filter-selector");
            input?.focus();
            input?.setSelectionRange(caret, caret);
        }
    }

    async saveCosmeticFilter() {
        if (!this.cosmeticFilterDraft?.previewing || this.app.state.isPrivate)
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
        const focusId = this.rootEl.contains(document.activeElement) ? document.activeElement.id : "";
        this.rootEl.textContent = "";
        const isDetached = !!this.app.state.ghostSnapshot;
        const canEditElement = !!this.app.state.selectedNodeId && !isDetached && !this.app.state.selectedPseudo;

        // Sticky toolbar: Filter, :hov, .cls, +
        const filterInput = el("input", {
            type: "search",
            id: "styles-filter",
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
                disabled: !canEditElement,
                onClick: () => { this.showHov = !this.showHov; this.render(); },
            }),
            el("button", {
                class: `chip-btn ${this.showCls ? "active" : ""}`,
                type: "button",
                text: ".cls",
                title: "Toggle element classes",
                disabled: !canEditElement,
                onClick: () => { this.showCls = !this.showCls; this.render(); },
            }),
            el("button", {
                class: "chip-btn",
                type: "button",
                text: "+",
                title: "New style rule in inspector stylesheet",
                disabled: !canEditElement,
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
                    disabled: !canEditElement,
                    checked: this.forcedStates.get(this.app.state.selectedNodeId)?.has(st.key),
                    onChange: () => this.toggleForcedState(st.key, cb),
                });
                grid.appendChild(el("label", {}, cb, st.label));
            }
            this.rootEl.appendChild(el("div", { class: "subdrawer-panel" }, grid));
        }

        // .cls Live Class Toggles
        if (this.showCls && canEditElement) {
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
        if (focusId)
            (document.getElementById(focusId) || filterInput).focus({ preventScroll: true });
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
        const list = el("div", { class: "pseudo-grid class-toggle-list" });
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
            id: "cosmetic-filter-selector",
            value: draft.selector,
            placeholder: "CSS selector to hide",
            onInput: e => {
                draft.selector = e.target.value;
                draft.previewing = false;
                this.previewCosmeticFilter();
            },
        });
        const proposedRule = `${draft.host || "*"}##${draft.selector}`;
        const panel = el(
            "div",
            { class: "subdrawer-panel" },
            el("div", { class: "subdrawer-title", text: "Create Cosmetic Filter" }),
            el("div", { class: "monospace muted subdrawer-code", text: proposedRule }),
            selectorInput,
            el("div", {
                class: "muted tabular subdrawer-status",
                text: draft.error ? "" : draft.matchCount === null
                    ? "Previewing matches…"
                    : `${draft.matchCount} matching element${draft.matchCount === 1 ? "" : "s"} · Scope: ${isPrivate ? "Private preview only (will not save to profile)" : "Regular profile custom filters"}`,
            }),
            draft.error ? el("div", { class: "subdrawer-error", text: draft.error }) : null,
            el(
                "div",
                { class: "button-row" },
                el("button", {
                    class: "chip-btn primary",
                    type: "button",
                    text: "Save Filter",
                    disabled: isPrivate || !draft.previewing,
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

    async toggleForcedState(stateKey, checkbox) {
        const nodeId = this.app.state.selectedNodeId;
        const enabled = checkbox.checked;
        checkbox.disabled = true;
        try {
            await sendEngineCommand("setForcedPseudoState", {
                nodeId,
                state: stateKey,
                enabled,
            });
            const states = this.forcedStates.get(nodeId) || new Set();
            if (enabled)
                states.add(stateKey);
            else
                states.delete(stateKey);
            if (states.size)
                this.forcedStates.set(nodeId, states);
            else
                this.forcedStates.delete(nodeId);
            if (this.app.state.selectedNodeId === nodeId)
                this.app.refreshSelectedNodeProperties();
        } catch (err) {
            checkbox.checked = !enabled;
            this.app.showToast(err.message);
        } finally {
            checkbox.disabled = false;
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
        const isDetached = !!this.app.state.ghostSnapshot;

        let lastInheritedId = null;
        let lastPseudoHeader = null;

        this.rules.forEach((rule, ruleIdx) => {
            const editable = !isDetached && !rule.isSystem
                && (rule.type === 100 || !!rule.sheetId)
                && !["content-blocker", "extension"].includes(rule.attribution?.kind);
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
            const editId = `style-${this.app.state.selectedNodeId}-${rule.inheritedNodeId ?? "self"}-${rule.ruleIdentity ?? "inline"}`;
            const selectorEl = rule.type !== 100 && editable
                ? el("button", {
                    class: "inline-editor-trigger selector-text", type: "button", text: selectorLabel,
                    id: `${editId}-selector`, "aria-label": `Edit selector ${selectorLabel}`,
                    onClick: e => this.beginInlineSelectorEdit(e.currentTarget, rule),
                })
                : el("span", { class: "selector-text", text: selectorLabel });
            const header = el(
                "div",
                { class: "style-rule-header" },
                selectorEl,
                sourceLabel
                    ? el(rule.ruleId?.type ? "button" : "span", {
                        class: "source-link",
                        type: rule.ruleId?.type ? "button" : undefined,
                        text: sourceLabel,
                        onClick: rule.ruleId?.type ? () => this.app.openStyleSource(rule) : undefined,
                    })
                    : null
            );
            ruleEl.appendChild(header);

            const declList = el("div", { class: "decl-list" });
            decls.forEach((decl, declIdx) => {
                const propName = String(decl.name || "");
                const propVal = String(decl.value || "");
                const isValid = decl.isValid !== false && decl.isNameValid !== false;
                const isDisabled = decl.disabled === true;

                const isOverridden = decl.overridden === true;
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
                    disabled: !editable,
                    "aria-label": `Toggle ${propName}`,
                    onChange: () => this.toggleDeclaration(rule, decl, declIdx, toggle.checked),
                });
                declRow.appendChild(toggle);

                const propSpan = el("span", { class: "decl-prop", text: propName });
                declRow.appendChild(propSpan);
                declRow.appendChild(document.createTextNode(": "));

                // Inline color swatch + WCAG/APCA contrast picker
                const parsedColor = parseCssColor(propVal.replace(/!important/i, "").trim());
                if (parsedColor && editable) {
                    const swatch = el("button", {
                        type: "button",
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
                const valSpan = editable
                    ? el("button", {
                        class: "inline-editor-trigger decl-val", type: "button", text: valText,
                        id: `${editId}-value-${declIdx}`, "aria-label": `Edit ${propName}, ${valText}`,
                        onClick: e => this.beginInlineValueEdit(e.currentTarget, rule, decl, declIdx),
                    })
                    : el("span", { class: "decl-val", text: valText });
                declRow.appendChild(valSpan);
                declRow.appendChild(document.createTextNode(";"));

                if (inactiveReason) {
                    declRow.appendChild(el("span", {
                        class: "inactive-chip",
                        text: "Inactive",
                        title: inactiveReason,
                    }));
                }

                if (hasStory) {
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
                if (hasStory && this.expandedStories.has(storyKey)) {
                    declList.appendChild(this.buildCascadeStoryBox(propName, rule, decl));
                }

                // Inline Color + Contrast Popover!
                if (this.openColorKey === storyKey && parsedColor) {
                    declList.appendChild(this.buildColorPopover(rule, decl, declIdx, parsedColor));
                }
            });

            // Quick "+ add declaration" trigger on editable rules
            if (editable) {
                const addRow = el("button", {
                    class: "add-decl-btn",
                    type: "button",
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
            box.appendChild(el("div", { class: "cascade-story-headline", text: story.reason || `Winning value for ${propName}` }));
            if (story.decidingStep)
                box.appendChild(el("div", { class: "muted cascade-story-step", text: `Deciding cascade step: ${story.decidingStep}` }));
            for (const cand of story.competitors || []) {
                box.appendChild(el(
                    "div",
                    { class: `cascade-candidate ${cand.won ? "winner" : ""}` },
                    el("span", { class: "monospace", text: `${cand.won ? "★ " : ""}${cand.selector || cand.origin}: ${cand.value}${cand.important ? " !important" : ""}` }),
                    el("span", { class: "muted", text: cand.reason || cand.layer || "" })
                ));
            }

        }
        return box;
    }

    buildColorPopover(rule, decl, declIdx, parsedColor) {
        const computed = this.app.state.lastComputedStyle || {};
        const bgColor = computed["background-color"];
        const bgImage = computed["background-image"] && computed["background-image"] !== "none";
        const blendMode = computed["mix-blend-mode"] && computed["mix-blend-mode"] !== "normal";
        const fontSizePx = parseFloat(computed["font-size"] || "14") || 14;
        const fontWeight = parseInt(computed["font-weight"] || "400", 10) || 400;

        const contrast = computeContrast(decl.name === "color" ? decl.value : computed.color, decl.name === "background-color" ? decl.value : bgColor, {
            hasBackgroundImage: bgImage,
            hasBlendMode: blendMode,
            fontSizePx,
            fontWeight,
        });

        const picker = el("input", {
            type: "color",
            value: rgbToHex(parsedColor),
            "aria-label": `Pick color for ${decl.name}`,
            onChange: e => this.commitDeclarationEdit(rule, decl, declIdx, decl.name, e.target.value + (decl.priority ? " !important" : "")),
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
            el("div", { class: "color-popover-row" }, picker, el("span", { class: "monospace", text: decl.value })),
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
        this.editText(selectorEl, next => {
            sendEngineCommand("editStyleRuleSelector", {
                nodeId: this.app.state.selectedNodeId,
                ruleIdentity: rule.ruleIdentity,
                sheetId: rule.sheetId,
                baseRevision: rule.sheetRevision,
                selectorText: next,
            }).then(res => {
                if (res?.conflict)
                    this.app.showToast("Stylesheet changed. Showing the current selector.");
                this.app.refreshSelectedNodeProperties();
            }).catch(err => this.app.showToast(err.message));
        });
    }

    beginInlineValueEdit(valSpan, rule, decl, declIdx) {
        this.editText(valSpan, next => this.commitDeclarationEdit(rule, decl, declIdx, decl.name, next), true);
    }

    editText(button, onCommit, stepNumbers = false) {
        const before = button.textContent;
        const input = el("input", {
            class: "dom-inline-input", type: "text", value: before, "aria-label": button.getAttribute("aria-label"),
        });
        button.replaceWith(input);
        input.focus();
        input.select();
        const finish = (commit, restoreFocus) => {
            if (!input.isConnected)
                return;
            const next = input.value.trim();
            input.removeEventListener("blur", onBlur);
            input.replaceWith(button);
            if (restoreFocus)
                button.focus();
            if (commit && next !== before)
                onCommit(next);
        };
        const onBlur = () => finish(true, false);
        input.addEventListener("blur", onBlur);
        input.addEventListener("keydown", e => {
            if (e.key === "Enter" || e.key === "Escape") {
                e.preventDefault();
                finish(e.key === "Enter", true);
            } else if (stepNumbers && (e.key === "ArrowUp" || e.key === "ArrowDown")) {
                // Numeric stepping with Up/Down (+Shift for 10, +Alt for 0.1)
                const match = input.value.match(/^(-?[\d.]+)([a-z%]*)$/i);
                if (match) {
                    e.preventDefault();
                    const step = e.shiftKey ? 10 : (e.altKey ? 0.1 : 1);
                    const delta = e.key === "ArrowUp" ? step : -step;
                    const nextNum = Number((parseFloat(match[1]) + delta).toFixed(2));
                    input.value = `${nextNum}${match[2]}`;
                }
            }
        });
    }

    beginAddDeclaration(declList, rule) {
        const input = el("input", { class: "dom-inline-input", type: "text", placeholder: "property: value" });
        declList.appendChild(input);
        input.focus();
        let finished = false;
        const finish = commit => {
            if (finished)
                return;
            finished = true;
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
                nodeId: rule.nodeId ?? rule.inheritedNodeId ?? this.app.state.selectedNodeId,
                isInline: rule.type === 100,
                ruleIdentity: rule.ruleIdentity || 0,
                sheetId: rule.sheetId,
                baseRevision: rule.sheetRevision || 0,
                declarationIndex: declIdx,
                property: decl.name,
                priority: decl.priority,
                value: decl.value,
                enabled,
            });
            this.app.refreshSelectedNodeProperties();
        } catch (err) {
            this.app.showToast(err.message);
        }
    }

    async commitDeclarationEdit(rule, decl, declIdx, name, value) {
        try {
            const res = await sendEngineCommand("editStyleDeclaration", {
                nodeId: rule.nodeId ?? rule.inheritedNodeId ?? this.app.state.selectedNodeId,
                isInline: rule.type === 100,
                ruleIdentity: rule.ruleIdentity || 0,
                sheetId: rule.sheetId,
                baseRevision: rule.sheetRevision || 0,
                declarationIndex: declIdx,
                property: name,
                oldProperty: decl.name,
                value: value.replace(/\s*!important\s*$/i, ""),
                priority: /!important\s*$/i.test(value) ? "important" : "",
                beforeValue: decl.value || "",
            });
            if (res?.conflict) {
                this.app.showToast("Stylesheet changed externally — refreshed latest revision.");
            } else {
                announce(`Updated ${name} to ${value}`);
            }
            this.app.refreshSelectedNodeProperties();
        } catch (err) {
            this.app.showToast(err.message);
        }
    }
}
