/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #13 Site Permissions (permissions-audit)
 * Scoped to caller's PermissionManager; verifies generation before revoking so expired/replaced grants stay safe.
 */

import { icon, siteIcon } from "../icons.js";

const PERMISSION_LABELS = { geolocation: "Location", microphone: "Microphone", notifications: "Notifications", camera: "Camera" };

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
    let state = { available: false, entries: [], defaults: {}, inheritsOrdinary: false };
    let permFilter = "all";
    root.classList.add("perm");

    const bar = el("div", "perm-bar");
    const filterSel = el("select", "dash-select perm-filter");
    filterSel.setAttribute("aria-label", "Filter permission type");
    for (const [val, lbl] of [["all", "All types"], ["geolocation", "Location"], ["microphone", "Microphone"], ["notifications", "Notifications"]]) {
        const opt = el("option", "", lbl);
        opt.value = val;
        filterSel.append(opt);
    }
    filterSel.addEventListener("change", () => {
        permFilter = filterSel.value;
        render();
    });

    const settingsBtn = el("button", "dash-btn ghost small");
    settingsBtn.type = "button";
    settingsBtn.append(document.createTextNode("Settings"), icon("external", 13));
    settingsBtn.addEventListener("click", () => api.openUrl("about:settings#permissions", false));
    bar.append(filterSel, settingsBtn);

    const summaryEl = el("div", "perm-summary");
    const listEl = el("ul", "dash-list perm-list");
    root.append(bar, summaryEl, listEl);

    function decisionLabel(decision) {
        return decision === "allow-once" ? "Allow once" : decision === "allow" ? "Allow" : "Block";
    }

    function render() {
        listEl.replaceChildren();
        summaryEl.replaceChildren();
        if (!state.available) {
            bar.classList.add("hidden");
            summaryEl.classList.add("hidden");
            api.setMeta("Unavailable");
            listEl.append(el("div", "dash-empty-card", "Site permissions are unavailable in this session."));
            return;
        }

        bar.classList.toggle("hidden", tier === "compact");
        summaryEl.classList.remove("hidden");
        const entries = (state.entries || []).filter(e => permFilter === "all" || e.permission === permFilter);
        api.setMeta(`${entries.length} rules`);

        const allowed = entries.filter(e => e.decision === "allow").length;
        const once = entries.filter(e => e.decision === "allow-once").length;
        const blocked = entries.length - allowed - once;
        const counts = el("div", "perm-counts tabular");
        const figure = el("span", "perm-total", String(entries.length));
        counts.append(figure, el("span", "dash-muted", entries.length === 1 ? "site rule" : "site rules"));
        summaryEl.append(counts);
        if (entries.length > 0) {
            const parts = [`${allowed} allowed`];
            if (once > 0)
                parts.push(`${once} once`);
            parts.push(`${blocked} blocked`);
            summaryEl.append(el("span", "dash-muted tabular perm-breakdown", parts.join(" · ")));
        }

        if (entries.length === 0) {
            listEl.append(el("div", "perm-empty",
                state.inheritsOrdinary
                    ? "No session-specific grants. Blocks from the default session still apply."
                    : "No site permission overrides recorded."));
            return;
        }

        for (const e of entries) {
            const li = el("li", "dash-list-item perm-row");
            const permName = PERMISSION_LABELS[e.permission] || e.permission;
            li.append(siteIcon({ className: "dash-site-icon", url: e.origin, label: e.origin.replace(/^[a-z]+:\/\/(www\.)?/i, "") }));
            li.append(el("span", "perm-origin dash-truncate", e.origin));
            li.append(el("span", "perm-kind dash-muted", permName));
            const dec = el("span", "perm-decision " + (e.decision === "allow" || e.decision === "allow-once" ? "is-allow" : "is-block"));
            dec.append(icon(e.decision === "allow" || e.decision === "allow-once" ? "check" : "close", 12), document.createTextNode(decisionLabel(e.decision)));
            li.append(dec);
            if (e.managed || e.source === "managed" || e.isManaged)
                li.append(el("span", "dash-badge", "Managed"));

            if (tier === "compact") {
                listEl.append(li);
                continue;
            }

            const actions = el("span", "dash-row-actions");
            const revBtn = el("button", "dash-btn ghost small");
            revBtn.type = "button";
            revBtn.textContent = "Revoke";
            revBtn.setAttribute("aria-label", `Revoke ${permName} for ${e.origin}`);
            revBtn.addEventListener("click", async () => {
                await api.request("permissions.revoke", {
                    origin: e.origin,
                    permission: e.permission,
                    generation: e.generation,
                });
                await load();
            });
            actions.append(revBtn);
            li.append(actions);
            listEl.append(li);
        }
    }

    async function load() {
        state = await api.request("permissions.list", {});
        render();
    }

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {},
    };
}
