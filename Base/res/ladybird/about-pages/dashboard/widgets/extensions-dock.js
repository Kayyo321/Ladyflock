/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #12 Extensions Dock (extensions-dock)
 * Ordinary browsing session only; private/named contexts receive no inventory or count.
 */

import { icon, siteIcon } from "../icons.js";

// The summary names an extension by `extension_id`.
function idOf(ext) {
    return ext.extension_id || ext.id;
}

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
    let state = { available: false, reason: "", summary: null };
    // Package icons by extension, kept across summaries that come without them, such as
    // the one a toggle returns.
    const icons = new Map();
    let iconRetry = null;
    let retriedIcons = false;
    root.classList.add("xdock");

    const listEl = el("ul", "xdock-grid");
    const footEl = el("div", "xdock-foot");
    root.append(listEl, footEl);

    async function setEnabled(ext) {
        const res = await api.request("extensions.setEnabled", {
            extensionId: idOf(ext),
            enabled: !ext.enabled,
        });
        if (res.summary) {
            state.summary = res.summary;
            render();
        } else {
            await load();
        }
    }

    function render() {
        listEl.replaceChildren();
        footEl.replaceChildren();
        listEl.classList.remove("is-empty");

        if (!state.available) {
            api.setMeta("Unavailable");
            listEl.classList.add("is-empty");
            listEl.append(el("li", "dash-empty-card", state.reason || "Extensions are unavailable in this session."));
            return;
        }

        const extensions = state.summary?.extensions || [];
        const enabledCount = extensions.filter(e => e.enabled).length;
        api.setMeta(`${enabledCount}/${extensions.length} enabled`);

        if (extensions.length === 0) {
            listEl.classList.add("is-empty");
            listEl.append(el("li", "dash-empty-card", "No extensions installed."));
        } else {
            const cap = tier === "compact" ? 4 : extensions.length;
            for (const ext of extensions.slice(0, cap)) {
                const name = ext.name || idOf(ext);
                const li = el("li", "xdock-item" + (ext.enabled ? " is-on" : ""));

                const mark = siteIcon({ className: "xdock-mark", label: name, favicon: icons.get(idOf(ext)) });
                const label = el("span", "xdock-name dash-truncate", name);
                const stateLbl = el("span", "xdock-state", ext.enabled ? "On" : "Off");

                if (tier === "compact") {
                    li.title = `${name}: ${ext.enabled ? "enabled" : "disabled"}`;
                    li.setAttribute("aria-label", li.title);
                    li.append(mark);
                    if (ext.enabled)
                        li.append(el("span", "xdock-pip"));
                } else {
                    const toggle = el("button", "xdock-toggle");
                    toggle.type = "button";
                    toggle.setAttribute("aria-pressed", String(Boolean(ext.enabled)));
                    toggle.setAttribute("aria-label", `${name}: ${ext.enabled ? "enabled, click to disable" : "disabled, click to enable"}`);
                    toggle.title = ext.enabled ? "Disable" : "Enable";
                    toggle.append(mark, label, stateLbl);
                    toggle.addEventListener("click", () => setEnabled(ext));
                    li.append(toggle);

                    if (tier === "expanded" && ext.enabled && ext.has_options) {
                        const optBtn = el("button", "dash-btn ghost icon small xdock-options");
                        optBtn.type = "button";
                        optBtn.setAttribute("aria-label", `${name} options`);
                        optBtn.title = "Options";
                        optBtn.append(icon("configure", 14));
                        optBtn.addEventListener("click", () => {
                            api.request("extensions.openOptions", { extensionId: idOf(ext) }).catch(() => load());
                        });
                        li.append(optBtn);
                    }
                }
                listEl.append(li);
            }
        }

        if (tier !== "compact") {
            const manageBtn = el("button", "dash-btn ghost small");
            manageBtn.type = "button";
            manageBtn.append(document.createTextNode("Manage add-ons"), icon("external", 13));
            manageBtn.addEventListener("click", () => api.openUrl("about:addons", false));
            footEl.append(manageBtn);
        }
    }

    async function load() {
        state = await api.request("extensions.list", {});
        const extensions = state.summary?.extensions || [];
        for (const ext of extensions) {
            if (ext.iconBase64Png)
                icons.set(idOf(ext), ext.iconBase64Png);
        }
        render();
        // The browser decodes a package icon the first time it is asked for, so an icon
        // still missing is asked for once more.
        if (!retriedIcons && extensions.some(ext => ext.icon_key && !icons.has(idOf(ext)))) {
            retriedIcons = true;
            iconRetry = setTimeout(() => load().catch(() => {}), 1200);
        }
    }

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            clearTimeout(iconRetry);
        },
    };
}
