/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #11 Privacy Summary (privacy-summary)
 * Scoped PrivacyPolicyCoordinator + PrivacyActivity snapshot.
 */

import { icon } from "../icons.js";

const MODE_LABELS = { allow: "Allow", offline: "Offline" };

function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className)
        node.className = className;
    if (text !== undefined)
        node.textContent = text;
    return node;
}

export function mount(root, api) {
    let tier = api.tier;
    let summary = null;
    root.classList.add("priv");

    function render() {
        root.replaceChildren();
        if (!summary || !summary.available) {
            api.setMeta("Unavailable");
            root.append(el("div", "dash-empty-card", "Privacy summary unavailable."));
            return;
        }

        const mode = summary.networkMode || "allow";
        api.setMeta(summary.identityName || "");

        const head = el("div", "priv-head");
        head.append(icon(mode === "offline" ? "mute" : "privacy-summary", 20));
        const text = el("div", "priv-mode");
        text.append(
            el("span", "priv-mode-name", MODE_LABELS[mode] || mode),
            el("span", "dash-muted", tier === "compact" ? "network" : "Network mode")
        );
        head.append(text);
        root.append(head);

        if (tier !== "compact" && summary.counters) {
            const c = summary.counters;
            const figures = el("dl", "priv-figures");
            const items = [
                ["Sanitized navigations", c.navigationsSanitized ?? 0],
                ["Tracking params removed", c.trackingParametersRemoved ?? 0],
                ["Scripting blocked", c.navigationsWithScriptingBlocked ?? 0],
            ];
            for (const [label, val] of items) {
                const cell = el("div", "priv-figure");
                const dd = el("dd", "priv-figure-value tabular", String(val));
                const dt = el("dt", "dash-muted", label);
                cell.append(dd, dt);
                figures.append(cell);
            }
            root.append(figures);
        }

        if (tier === "expanded") {
            const details = el("ul", "dash-list priv-details");
            const rows = [
                ["Route", summary.route || "Direct"],
                ["Local DNS", summary.localDNS ? "Allowed" : "Blocked"],
                ["WebRTC", summary.webRTC ? "Allowed" : "Blocked"],
            ];
            for (const [label, val] of rows) {
                const li = el("li", "dash-row-between priv-detail");
                li.append(el("span", "dash-muted", label), el("span", "", val));
                details.append(li);
            }
            root.append(details);

            const openBtn = el("button", "dash-btn ghost priv-open");
            openBtn.type = "button";
            openBtn.append(document.createTextNode("Open Privacy"), icon("external", 13));
            openBtn.addEventListener("click", () => api.openUrl("about:privacy", false));
            root.append(openBtn);
        }
    }

    async function load() {
        summary = await api.request("privacy.summary", {});
        render();
    }

    const offPrivacy = api.onEvent("privacyChanged", () => load());

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            offPrivacy();
        },
    };
}
