/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #28 URL Inspector (url-inspector)
 * Pure in-document HTTP(S) URL parser and query parameter pruner.
 * Zero network, DNS, navigation, or favicon calls.
 * Masks embedded credentials and refuses to copy credentials into edited links.
 * Preserves order, duplicate keys, and original escaping of retained query fields.
 */

import { icon } from "../icons.js";

function parseRawQueryPairs(rawSearch) {
    if (!rawSearch || !rawSearch.startsWith("?"))
        return [];
    const body = rawSearch.slice(1);
    if (!body)
        return [];
    return body.split("&").map((rawPart, index) => {
        const eq = rawPart.indexOf("=");
        const rawKey = eq >= 0 ? rawPart.slice(0, eq) : rawPart;
        const rawVal = eq >= 0 ? rawPart.slice(eq + 1) : null;
        let decKey = rawKey;
        let decVal = rawVal;
        try {
            decKey = decodeURIComponent(rawKey.replace(/\+/g, " "));
        } catch {}
        try {
            if (rawVal !== null)
                decVal = decodeURIComponent(rawVal.replace(/\+/g, " "));
        } catch {}
        return { index, rawPart, rawKey, rawVal, decKey, decVal, kept: true };
    });
}

export function mount(root, api) {
    let tier = api.tier;
    let parsed = null;
    let pairs = [];

    root.classList.add("urlx");

    // The paste field is the hero until there is something to read.
    const field = document.createElement("label");
    field.className = "urlx-field";
    field.append(icon("url-inspector", 15));
    const input = document.createElement("input");
    input.type = "text";
    input.className = "urlx-input mono";
    input.placeholder = "Paste an http:// or https:// URL…";
    input.spellcheck = false;
    input.setAttribute("aria-label", "URL to inspect");
    field.append(input);

    const infoEl = document.createElement("div");
    infoEl.className = "urlx-info";

    const scrollEl = document.createElement("div");
    scrollEl.className = "urlx-scroll";

    const partsEl = document.createElement("dl");
    partsEl.className = "urlx-parts";

    const paramsList = document.createElement("ul");
    paramsList.className = "dash-list urlx-params";

    scrollEl.append(partsEl, paramsList);

    const editedWrap = document.createElement("div");
    editedWrap.className = "urlx-edited hidden";

    root.append(field, infoEl, scrollEl, editedWrap);
    api.setMeta("Offline");

    function buildEditedUrl() {
        if (!parsed)
            return "";
        // Strip embedded credentials whenever building a copyable link.
        const originPart = `${parsed.protocol}//${parsed.host}`;
        const retained = pairs.filter(p => p.kept).map(p => p.rawPart);
        const query = retained.length ? `?${retained.join("&")}` : "";
        return `${originPart}${parsed.pathname}${query}${parsed.hash}`;
    }

    function addPart(label, value) {
        const dt = document.createElement("dt");
        dt.className = "urlx-part-label";
        dt.textContent = label;
        const dd = document.createElement("dd");
        dd.className = "urlx-part-value mono dash-truncate";
        dd.textContent = value;
        dd.title = value;
        partsEl.append(dt, dd);
    }

    function render() {
        infoEl.replaceChildren();
        partsEl.replaceChildren();
        paramsList.replaceChildren();
        editedWrap.replaceChildren();
        root.classList.toggle("is-empty", !parsed);

        const raw = input.value.trim();
        if (!raw) {
            editedWrap.classList.add("hidden");
            const msg = document.createElement("span");
            msg.className = "urlx-hint";
            msg.textContent = "Inspect components and deselect tracking parameters offline.";
            infoEl.append(msg);
            return;
        }

        if (!parsed) {
            editedWrap.classList.add("hidden");
            const err = document.createElement("span");
            err.className = "urlx-error";
            err.textContent = "Enter a valid http:// or https:// URL.";
            infoEl.append(err);
            return;
        }

        const hasCreds = Boolean(parsed.username || parsed.password);
        const hostRow = document.createElement("div");
        hostRow.className = "urlx-host-row";
        const hostLbl = document.createElement("span");
        hostLbl.className = "urlx-host dash-truncate";
        hostLbl.textContent = parsed.hostname;
        hostLbl.title = parsed.hostname;
        hostRow.append(hostLbl);
        if (parsed.port) {
            const portLbl = document.createElement("span");
            portLbl.className = "urlx-port mono";
            portLbl.textContent = `:${parsed.port}`;
            hostRow.append(portLbl);
        }
        const isHttps = parsed.protocol === "https:";
        const protoBadge = document.createElement("span");
        protoBadge.className = `dash-badge urlx-proto ${isHttps ? "ok" : "warn"}`;
        if (isHttps)
            protoBadge.append(icon("private", 11));
        const protoText = document.createElement("span");
        protoText.textContent = parsed.protocol.replace(":", "").toUpperCase();
        protoBadge.append(protoText);
        hostRow.append(protoBadge);
        infoEl.append(hostRow);

        if (hasCreds) {
            const credWarn = document.createElement("span");
            credWarn.className = "dash-badge warn urlx-cred";
            credWarn.setAttribute("role", "status");
            credWarn.textContent = "Embedded credentials masked (omitted from copy)";
            infoEl.append(credWarn);
        }

        if (tier === "expanded") {
            addPart("Path", parsed.pathname);
            if (parsed.hash)
                addPart("Fragment", parsed.hash);
            if (pairs.length)
                addPart("Query", `${pairs.length} parameter${pairs.length === 1 ? "" : "s"}`);
        } else if (tier !== "compact") {
            addPart("Path", `${parsed.pathname}${parsed.hash}`);
        }

        for (const p of pairs) {
            const li = document.createElement("li");
            li.className = "dash-list-item urlx-param";
            const cb = document.createElement("input");
            cb.type = "checkbox";
            cb.checked = p.kept;
            cb.disabled = tier === "compact";
            cb.setAttribute("aria-label", `Keep parameter ${p.decKey}`);
            cb.addEventListener("change", () => {
                p.kept = cb.checked;
                li.classList.toggle("is-dropped", !p.kept);
                renderEdited();
            });
            li.classList.toggle("is-dropped", !p.kept);
            const k = document.createElement("span");
            k.className = "urlx-key mono dash-truncate";
            k.textContent = p.decKey;
            const v = document.createElement("span");
            v.className = "urlx-val mono dash-truncate";
            v.textContent = p.decVal !== null ? `= ${p.decVal}` : "";
            v.title = p.decVal !== null ? p.decVal : "";
            li.append(cb, k, v);
            paramsList.append(li);
        }

        renderEdited();
    }

    function renderEdited() {
        editedWrap.replaceChildren();
        if (!parsed || tier !== "expanded") {
            editedWrap.classList.add("hidden");
            return;
        }
        editedWrap.classList.remove("hidden");
        const edited = buildEditedUrl();
        const preview = document.createElement("input");
        preview.type = "text";
        preview.readOnly = true;
        preview.className = "dash-input mono urlx-preview";
        preview.value = edited;
        preview.setAttribute("aria-label", "Edited URL preview");
        const copyBtn = document.createElement("button");
        copyBtn.type = "button";
        copyBtn.className = "dash-btn primary small";
        copyBtn.textContent = "Copy clean URL";
        copyBtn.addEventListener("click", async () => {
            try {
                await navigator.clipboard.writeText(edited);
                copyBtn.textContent = "Copied";
                setTimeout(() => {
                    copyBtn.textContent = "Copy clean URL";
                }, 1500);
            } catch {
                preview.select();
            }
        });
        editedWrap.append(preview, copyBtn);
    }

    input.addEventListener("input", () => {
        const raw = input.value.trim();
        parsed = null;
        pairs = [];
        if (raw) {
            try {
                const u = new URL(raw);
                if (u.protocol === "http:" || u.protocol === "https:") {
                    parsed = u;
                    pairs = parseRawQueryPairs(u.search);
                }
            } catch {
                parsed = null;
            }
        }
        render();
    });

    render();

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            input.value = "";
            parsed = null;
            pairs = [];
        },
    };
}
