/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #26 Encoding and Hash Workbench (dev-codec)
 * Strictly memory-only (never persisted, logged, or synced). Bounded 1 MiB input.
 * Base64 (UTF-8), URL encode/decode, JSON format/minify, JWT (unverified), UUID v4, SHA-256.
 */

import { icon } from "../icons.js";

const MAX_CODEC_BYTES = 1024 * 1024;

function utf8ToBase64(str) {
    const bytes = new TextEncoder().encode(str);
    let bin = "";
    for (let i = 0; i < bytes.length; ++i)
        bin += String.fromCharCode(bytes[i]);
    return btoa(bin);
}

function base64ToUtf8(b64) {
    const clean = b64.replace(/\s+/g, "").replace(/-/g, "+").replace(/_/g, "/");
    if (!/^[A-Za-z0-9+/]*={0,2}$/.test(clean) || clean.length % 4 === 1)
        throw new Error("Invalid Base64 input.");
    const padded = clean + "=".repeat((4 - (clean.length % 4)) % 4);
    const bin = atob(padded);
    const bytes = new Uint8Array(bin.length);
    for (let i = 0; i < bin.length; ++i)
        bytes[i] = bin.charCodeAt(i);
    return new TextDecoder("utf-8", { fatal: true }).decode(bytes);
}

function checkJsonDepth(val, depth = 0) {
    if (depth > 32)
        throw new Error("JSON nesting exceeds maximum depth (32).");
    if (Array.isArray(val)) {
        for (const item of val)
            checkJsonDepth(item, depth + 1);
    } else if (val && typeof val === "object") {
        for (const k of Object.keys(val))
            checkJsonDepth(val[k], depth + 1);
    }
}

async function sha256Hex(str) {
    if (!globalThis.crypto?.subtle?.digest)
        throw new Error("Web Crypto SHA-256 is unavailable in this context.");
    const data = new TextEncoder().encode(str);
    const hash = await crypto.subtle.digest("SHA-256", data);
    return [...new Uint8Array(hash)].map(b => b.toString(16).padStart(2, "0")).join("");
}

function generateUuidV4() {
    if (globalThis.crypto?.randomUUID)
        return crypto.randomUUID();
    const bytes = new Uint8Array(16);
    crypto.getRandomValues(bytes);
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    const hex = [...bytes].map(b => b.toString(16).padStart(2, "0")).join("");
    return `${hex.slice(0, 8)}-${hex.slice(8, 12)}-${hex.slice(12, 16)}-${hex.slice(16, 20)}-${hex.slice(20)}`;
}

export function mount(root, api) {
    let tier = api.tier;

    root.classList.add("codec");

    const topRow = document.createElement("div");
    topRow.className = "codec-bar";

    const modeSel = document.createElement("select");
    modeSel.className = "dash-select codec-mode";
    modeSel.setAttribute("aria-label", "Codec operation");
    const ops = [
        ["b64-enc", "Base64 encode"],
        ["b64-dec", "Base64 decode"],
        ["url-enc", "URL encode"],
        ["url-dec", "URL decode"],
        ["json-fmt", "JSON format"],
        ["json-min", "JSON minify"],
        ["jwt-dec", "JWT payload (unverified)"],
        ["sha256", "SHA-256 hash"],
        ["uuid", "Generate UUID v4"],
    ];
    for (const [val, lbl] of ops) {
        const opt = document.createElement("option");
        opt.value = val;
        opt.textContent = lbl;
        modeSel.append(opt);
    }

    const actions = document.createElement("div");
    actions.className = "codec-actions";

    const smallIconButton = (glyph, label) => {
        const btn = document.createElement("button");
        btn.type = "button";
        btn.className = "dash-btn ghost icon small";
        btn.setAttribute("aria-label", label);
        btn.title = label;
        btn.append(icon(glyph, 14));
        return btn;
    };

    const clearBtn = smallIconButton("close", "Clear input and output");

    const runBtn = document.createElement("button");
    runBtn.type = "button";
    runBtn.className = "dash-btn primary small codec-run";
    runBtn.textContent = "Run";

    actions.append(clearBtn, runBtn);
    topRow.append(modeSel, actions);

    const noteEl = document.createElement("div");
    noteEl.className = "codec-note";
    noteEl.append(icon("private", 13));
    const noteText = document.createElement("span");
    noteText.textContent = "Memory only · never saved to disk";
    noteEl.append(noteText);

    const jwtNote = document.createElement("div");
    jwtNote.className = "dash-notice codec-jwt hidden";
    jwtNote.setAttribute("role", "note");
    const jwtText = document.createElement("span");
    const jwtLong = document.createElement("span");
    jwtLong.className = "codec-jwt-long";
    jwtLong.textContent = "JWT signature is not verified; treat the claims as untrusted";
    const jwtShort = document.createElement("span");
    jwtShort.className = "codec-jwt-short";
    jwtShort.textContent = "JWT signature not verified";
    jwtText.append(jwtLong, jwtShort);
    jwtNote.append(jwtText);

    const panes = document.createElement("div");
    panes.className = "codec-panes";

    const inputArea = document.createElement("textarea");
    inputArea.className = "codec-pane codec-in mono";
    inputArea.placeholder = "Input…";
    inputArea.spellcheck = false;
    inputArea.setAttribute("aria-label", "Codec input");

    const outputArea = document.createElement("textarea");
    outputArea.className = "codec-pane codec-out mono";
    outputArea.readOnly = true;
    outputArea.placeholder = "Output…";
    outputArea.setAttribute("aria-label", "Codec output");

    panes.append(inputArea, outputArea);

    root.append(topRow, jwtNote, panes, noteEl);
    api.setMeta("Memory only");

    function syncFace() {
        jwtNote.classList.toggle("hidden", modeSel.value !== "jwt-dec");
        outputArea.classList.toggle("is-error", outputArea.value.startsWith("Error:"));
    }

    async function run() {
        const op = modeSel.value;
        const raw = inputArea.value;
        if (new TextEncoder().encode(raw).length > MAX_CODEC_BYTES) {
            outputArea.value = "Error: Input exceeds 1 MiB limit.";
            return;
        }
        try {
            if (op === "b64-enc") {
                outputArea.value = utf8ToBase64(raw);
            } else if (op === "b64-dec") {
                outputArea.value = base64ToUtf8(raw);
            } else if (op === "url-enc") {
                outputArea.value = encodeURIComponent(raw);
            } else if (op === "url-dec") {
                outputArea.value = decodeURIComponent(raw);
            } else if (op === "json-fmt" || op === "json-min") {
                const parsed = JSON.parse(raw);
                checkJsonDepth(parsed);
                outputArea.value = op === "json-fmt" ? JSON.stringify(parsed, null, 2) : JSON.stringify(parsed);
            } else if (op === "jwt-dec") {
                const parts = raw.trim().split(".");
                if (parts.length < 2)
                    throw new Error("Expected header.payload[.signature]");
                const header = JSON.parse(base64ToUtf8(parts[0]));
                const payload = JSON.parse(base64ToUtf8(parts[1]));
                outputArea.value = `[UNVERIFIED SIGNATURE]\nHeader:\n${JSON.stringify(header, null, 2)}\n\nPayload:\n${JSON.stringify(payload, null, 2)}`;
            } else if (op === "sha256") {
                outputArea.value = await sha256Hex(raw);
            } else if (op === "uuid") {
                outputArea.value = generateUuidV4();
            }
        } catch (err) {
            outputArea.value = `Error: ${err.message}`;
        }
    }

    runBtn.addEventListener("click", async () => {
        await run();
        syncFace();
    });
    modeSel.addEventListener("change", async () => {
        syncFace();
        if (modeSel.value === "uuid" || inputArea.value) {
            await run();
            syncFace();
        }
    });
    clearBtn.addEventListener("click", () => {
        inputArea.value = "";
        outputArea.value = "";
        syncFace();
    });
    syncFace();

    return {
        onUpdate(next) {
            tier = next.tier;
            noteEl.classList.toggle("hidden", tier === "compact");
        },
        unmount() {
            inputArea.value = "";
            outputArea.value = "";
        },
    };
}
