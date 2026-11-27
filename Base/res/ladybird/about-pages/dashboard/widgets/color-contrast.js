/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #25 Color and Contrast (color-contrast)
 * Opaque sRGB hex/rgb/hsl only; exact unrounded WCAG 2.2 relative-luminance contrast check.
 */

import { icon } from "../icons.js";

function parseOpaqueSrgb(raw) {
    const s = (raw || "").trim();
    if (!s)
        return null;

    // Reject alpha formats explicitly until compositing is specified
    if (/^rgba\(|^hsla\(|^oklch\(|^lch\(|^lab\(|^color\(/i.test(s))
        return { error: "Only opaque sRGB hex, rgb(), and hsl() colors are supported." };

    const hex3 = /^#([0-9a-f]{3})$/i.exec(s);
    if (hex3) {
        const h = hex3[1];
        return {
            r: parseInt(h[0] + h[0], 16),
            g: parseInt(h[1] + h[1], 16),
            b: parseInt(h[2] + h[2], 16),
        };
    }
    const hex6 = /^#([0-9a-f]{6})$/i.exec(s);
    if (hex6) {
        const h = hex6[1];
        return {
            r: parseInt(h.slice(0, 2), 16),
            g: parseInt(h.slice(2, 4), 16),
            b: parseInt(h.slice(4, 6), 16),
        };
    }
    if (/^#[0-9a-f]{4}$|^#[0-9a-f]{8}$/i.test(s))
        return { error: "Alpha hex colors are not supported; enter an opaque 3- or 6-digit hex color." };

    const rgb = /^rgb\(\s*(\d{1,3})\s*,\s*(\d{1,3})\s*,\s*(\d{1,3})\s*\)$/i.exec(s);
    if (rgb) {
        const [r, g, b] = [Number(rgb[1]), Number(rgb[2]), Number(rgb[3])];
        if (r > 255 || g > 255 || b > 255)
            return { error: "RGB channels must be 0–255." };
        return { r, g, b };
    }

    const hsl = /^hsl\(\s*(\d{1,3})\s*,\s*(\d{1,3})%\s*,\s*(\d{1,3})%\s*\)$/i.exec(s);
    if (hsl) {
        const h = Number(hsl[1]) % 360;
        const sat = Math.min(100, Number(hsl[2])) / 100;
        const lit = Math.min(100, Number(hsl[3])) / 100;
        const c = (1 - Math.abs(2 * lit - 1)) * sat;
        const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
        const m = lit - c / 2;
        let r1 = 0, g1 = 0, b1 = 0;
        if (h < 60) { r1 = c; g1 = x; }
        else if (h < 120) { r1 = x; g1 = c; }
        else if (h < 180) { g1 = c; b1 = x; }
        else if (h < 240) { g1 = x; b1 = c; }
        else if (h < 300) { r1 = x; b1 = c; }
        else { r1 = c; b1 = x; }
        return {
            r: Math.round((r1 + m) * 255),
            g: Math.round((g1 + m) * 255),
            b: Math.round((b1 + m) * 255),
        };
    }

    return { error: "Unrecognized color. Use #RRGGBB, rgb(r,g,b), or hsl(h,s%,l%)." };
}

function toHex({ r, g, b }) {
    return "#" + [r, g, b].map(v => v.toString(16).padStart(2, "0")).join("").toUpperCase();
}

function relativeLuminance({ r, g, b }) {
    const lin = c => {
        const s = c / 255;
        return s <= 0.04045 ? s / 12.92 : Math.pow((s + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(r) + 0.7152 * lin(g) + 0.0722 * lin(b);
}

function contrastRatio(c1, c2) {
    const l1 = relativeLuminance(c1);
    const l2 = relativeLuminance(c2);
    const hi = Math.max(l1, l2);
    const lo = Math.min(l1, l2);
    return (hi + 0.05) / (lo + 0.05);
}

function suggestPassingForeground(fg, bg, minRatio = 4.5) {
    const bgLum = relativeLuminance(bg);
    const step = bgLum > 0.5 ? -8 : 8;
    let cand = { ...fg };
    for (let i = 0; i < 32; ++i) {
        if (contrastRatio(cand, bg) >= minRatio)
            return cand;
        cand = {
            r: Math.max(0, Math.min(255, cand.r + step)),
            g: Math.max(0, Math.min(255, cand.g + step)),
            b: Math.max(0, Math.min(255, cand.b + step)),
        };
    }
    return bgLum > 0.5 ? { r: 0, g: 0, b: 0 } : { r: 255, g: 255, b: 255 };
}

export function mount(root, api) {
    let tier = api.tier;
    let swatches = [];

    root.classList.add("contrast");

    // The hero: one sample swatch painted in the chosen colors, beside the ratio and its verdicts.
    const mainRow = document.createElement("div");
    mainRow.className = "contrast-main";

    const previewBox = document.createElement("div");
    previewBox.className = "contrast-swatch";

    const side = document.createElement("div");
    side.className = "contrast-side";

    const ratioEl = document.createElement("div");
    ratioEl.className = "dash-figure contrast-ratio";

    const ratioRow = document.createElement("div");
    ratioRow.className = "contrast-badges";

    side.append(ratioEl, ratioRow);
    mainRow.append(previewBox, side);

    const errEl = document.createElement("div");
    errEl.className = "contrast-error hidden";
    errEl.setAttribute("role", "status");

    const mkField = (label, value) => {
        const wrap = document.createElement("label");
        wrap.className = "contrast-field";
        wrap.title = label;
        const chip = document.createElement("span");
        chip.className = "contrast-chip";
        chip.setAttribute("aria-hidden", "true");
        const input = document.createElement("input");
        input.type = "text";
        input.className = "contrast-input mono";
        input.value = value;
        input.spellcheck = false;
        input.setAttribute("aria-label", label);
        wrap.append(chip, input);
        return { wrap, chip, input };
    };

    const fgField = mkField("Text color", "#111111");
    const bgField = mkField("Background color", "#FFFFFF");
    const fgInput = fgField.input;
    const bgInput = bgField.input;

    const swapBtn = document.createElement("button");
    swapBtn.type = "button";
    swapBtn.className = "dash-btn ghost icon small contrast-swap";
    swapBtn.setAttribute("aria-label", "Swap colors");
    swapBtn.title = "Swap colors";
    swapBtn.append(icon("swap", 14));
    swapBtn.addEventListener("click", () => {
        const tmp = fgInput.value;
        fgInput.value = bgInput.value;
        bgInput.value = tmp;
        render();
    });

    const inputsRow = document.createElement("div");
    inputsRow.className = "contrast-fields";
    inputsRow.append(fgField.wrap, swapBtn, bgField.wrap);

    const suggestRow = document.createElement("div");
    suggestRow.className = "dash-notice contrast-suggest hidden";

    const swatchRow = document.createElement("div");
    swatchRow.className = "contrast-pairs";

    root.append(mainRow, errEl, inputsRow, suggestRow, swatchRow);

    function render() {
        const fg = parseOpaqueSrgb(fgInput.value);
        const bg = parseOpaqueSrgb(bgInput.value);

        fgField.chip.style.backgroundColor = fg && !fg.error ? toHex(fg) : "";
        bgField.chip.style.backgroundColor = bg && !bg.error ? toHex(bg) : "";
        fgField.wrap.classList.toggle("is-invalid", Boolean(fg?.error) || !fg);
        bgField.wrap.classList.toggle("is-invalid", Boolean(bg?.error) || !bg);

        if (!fg || fg.error || !bg || bg.error) {
            errEl.textContent = fg?.error || bg?.error || "Enter two opaque sRGB colors.";
            errEl.classList.remove("hidden");
            mainRow.classList.add("hidden");
            ratioRow.replaceChildren();
            suggestRow.classList.add("hidden");
            api.setMeta("Invalid");
            return;
        }

        errEl.classList.add("hidden");
        mainRow.classList.remove("hidden");

        // Compare UNROUNDED ratio against published WCAG thresholds so 4.499 never passes by rounding!
        const ratio = contrastRatio(fg, bg);
        const passNormalAA = ratio >= 4.5;
        const passNormalAAA = ratio >= 7.0;
        const passLargeAA = ratio >= 3.0;

        const displayRatio = `${(Math.floor(ratio * 100) / 100).toFixed(2)}:1`;
        api.setMeta(displayRatio);

        ratioEl.replaceChildren();
        ratioEl.setAttribute("aria-label", `Contrast ratio ${displayRatio}`);
        const ratioNum = document.createElement("span");
        ratioNum.textContent = displayRatio.slice(0, -2);
        const ratioUnit = document.createElement("span");
        ratioUnit.className = "dash-figure-unit";
        ratioUnit.textContent = ":1";
        ratioEl.append(ratioNum, ratioUnit);

        previewBox.style.color = toHex(fg);
        previewBox.style.backgroundColor = toHex(bg);
        previewBox.replaceChildren();
        const sampleBig = document.createElement("span");
        sampleBig.className = "contrast-sample-aa";
        sampleBig.textContent = "Aa";
        const sampleNormal = document.createElement("span");
        sampleNormal.className = "contrast-sample-normal";
        sampleNormal.textContent = tier === "expanded" ? "Normal text (14 px) · Aa Bb Cc 123" : "Normal text · 123";
        previewBox.append(sampleBig, sampleNormal);
        if (tier === "expanded") {
            const sampleLarge = document.createElement("span");
            sampleLarge.className = "contrast-sample-large";
            sampleLarge.textContent = "Large bold text (18 px)";
            previewBox.append(sampleLarge);
        }

        ratioRow.replaceChildren();
        const mkBadge = (lbl, full, ok) => {
            const b = document.createElement("span");
            b.className = `dash-badge contrast-badge ${ok ? "ok" : "err"}`;
            b.title = `${full}: ${ok ? "Pass" : "Fail"}`;
            b.append(icon(ok ? "check" : "close", 12));
            const t = document.createElement("span");
            t.textContent = lbl;
            const verdict = document.createElement("span");
            verdict.className = "contrast-verdict";
            verdict.textContent = ok ? " Pass" : " Fail";
            b.append(t, verdict);
            return b;
        };
        ratioRow.append(
            mkBadge("AA", "AA normal text, 4.5:1", passNormalAA),
            mkBadge("AAA", "AAA normal text, 7.0:1", passNormalAAA),
            mkBadge("AA large", "AA large text, 3.0:1", passLargeAA)
        );

        suggestRow.replaceChildren();
        if (!passNormalAA && tier === "expanded") {
            const suggested = suggestPassingForeground(fg, bg, 4.5);
            const hex = toHex(suggested);
            const txt = document.createElement("span");
            txt.className = "contrast-suggest-text";
            const sugChip = document.createElement("span");
            sugChip.className = "contrast-chip";
            sugChip.style.backgroundColor = hex;
            sugChip.setAttribute("aria-hidden", "true");
            const sugLabel = document.createElement("span");
            sugLabel.textContent = "Suggested AA text color ";
            const sugHex = document.createElement("span");
            sugHex.className = "mono";
            sugHex.textContent = hex;
            txt.append(sugChip, sugLabel, sugHex);
            const applyBtn = document.createElement("button");
            applyBtn.type = "button";
            applyBtn.className = "dash-btn primary small";
            applyBtn.textContent = "Use";
            applyBtn.addEventListener("click", () => {
                fgInput.value = hex;
                render();
            });
            suggestRow.append(txt, applyBtn);
            suggestRow.classList.remove("hidden");
        } else {
            suggestRow.classList.add("hidden");
        }

        swatchRow.replaceChildren();
        swatchRow.classList.toggle("hidden", tier !== "expanded");
        if (tier === "expanded") {
            const saveBtn = document.createElement("button");
            saveBtn.type = "button";
            saveBtn.className = "dash-btn small";
            saveBtn.append(icon("plus", 13));
            const saveLbl = document.createElement("span");
            saveLbl.textContent = "Save pair";
            saveBtn.append(saveLbl);
            saveBtn.addEventListener("click", async () => {
                const val = { fg: toHex(fg), bg: toHex(bg) };
                const res = await api.request("items.save", { collection: "color-swatches", value: val });
                swatches.unshift({ itemId: res.itemId, value: val });
                render();
            });
            swatchRow.append(saveBtn);
            for (const sw of swatches.slice(0, 6)) {
                const chip = document.createElement("button");
                chip.type = "button";
                chip.className = "contrast-pair";
                chip.style.color = sw.value.fg;
                chip.style.backgroundColor = sw.value.bg;
                chip.textContent = "Aa";
                const label = `Use ${sw.value.fg} on ${sw.value.bg}`;
                chip.title = label;
                chip.setAttribute("aria-label", label);
                chip.addEventListener("click", () => {
                    fgInput.value = sw.value.fg;
                    bgInput.value = sw.value.bg;
                    render();
                });
                swatchRow.append(chip);
            }
        }
    }

    fgInput.addEventListener("input", render);
    bgInput.addEventListener("input", render);

    api.request("items.list", { collection: "color-swatches", limit: 12 })
        .then(res => {
            swatches = res.items || [];
            render();
        })
        .catch(() => render());

    render();

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {},
    };
}
