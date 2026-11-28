/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #24 Calculator and Units (calculator)
 * Bounded recursive-descent arithmetic parser (no eval) + unit families:
 * temperature (C/F/K), data (decimal B/KB/MB/GB & binary KiB/MiB/GiB),
 * length (m/km/cm/mm/in/ft/mi), CSS (px/rem/em with configurable remBase).
 */

function evaluateArithmetic(expr) {
    const tokens = [];
    let i = 0;
    const s = expr.trim();
    if (!s || s.length > 256)
        throw new Error("Expression too long");

    while (i < s.length) {
        const ch = s[i];
        if (/\s/.test(ch)) {
            ++i;
            continue;
        }
        if (/[0-9.]/.test(ch)) {
            let numStr = "";
            while (i < s.length && /[0-9._]/.test(s[i])) {
                if (s[i] !== "_")
                    numStr += s[i];
                ++i;
            }
            const val = Number(numStr);
            if (!Number.isFinite(val))
                throw new Error("Invalid number");
            tokens.push({ type: "num", value: val });
            continue;
        }
        if ("+-*/%^()".includes(ch)) {
            tokens.push({ type: "op", value: ch });
            ++i;
            continue;
        }
        throw new Error(`Unexpected "${ch}"`);
    }

    let pos = 0;
    const peek = () => tokens[pos] || null;
    const next = () => tokens[pos++] || null;

    function parseExpr() {
        let left = parseTerm();
        while (peek() && (peek().value === "+" || peek().value === "-")) {
            const op = next().value;
            const right = parseTerm();
            left = op === "+" ? left + right : left - right;
        }
        return left;
    }

    function parseTerm() {
        let left = parsePower();
        while (peek() && (peek().value === "*" || peek().value === "/" || peek().value === "%")) {
            const op = next().value;
            const right = parsePower();
            if ((op === "/" || op === "%") && right === 0)
                throw new Error("Division by zero");
            left = op === "*" ? left * right : op === "/" ? left / right : left % right;
        }
        return left;
    }

    function parsePower() {
        let base = parseUnary();
        if (peek() && peek().value === "^") {
            next();
            const exp = parsePower();
            base = Math.pow(base, exp);
        }
        return base;
    }

    function parseUnary() {
        if (peek() && peek().value === "+") {
            next();
            return parseUnary();
        }
        if (peek() && peek().value === "-") {
            next();
            return -parseUnary();
        }
        return parsePrimary();
    }

    function parsePrimary() {
        const tok = peek();
        if (!tok)
            throw new Error("Incomplete expression");
        if (tok.type === "num") {
            next();
            return tok.value;
        }
        if (tok.value === "(") {
            next();
            const val = parseExpr();
            if (!peek() || peek().value !== ")")
                throw new Error("Missing closing parenthesis");
            next();
            return val;
        }
        throw new Error("Invalid syntax");
    }

    const result = parseExpr();
    if (pos !== tokens.length)
        throw new Error("Unexpected trailing input");
    if (!Number.isFinite(result))
        throw new Error("Result out of range");
    return result;
}

const DATA_UNITS = {
    b: 1,
    kb: 1e3,
    mb: 1e6,
    gb: 1e9,
    tb: 1e12,
    kib: 1024,
    mib: 1024 ** 2,
    gib: 1024 ** 3,
    tib: 1024 ** 4,
};

const LENGTH_UNITS = {
    mm: 0.001,
    cm: 0.01,
    m: 1,
    km: 1000,
    in: 0.0254,
    ft: 0.3048,
    mi: 1609.344,
};

function tryUnitConversion(raw, remBase) {
    const m = /^\s*([+-]?\d+(?:\.\d+)?)\s*°?\s*([A-Za-z]+)\s+(?:to|in|->|→)\s+°?\s*([A-Za-z]+)\s*$/i.exec(raw);
    if (!m)
        return null;
    const val = Number(m[1]);
    const from = m[2].toLowerCase();
    const to = m[3].toLowerCase();
    if (!Number.isFinite(val))
        return null;

    // Temperature
    const tempAlias = { c: "c", celsius: "c", f: "f", fahrenheit: "f", k: "k", kelvin: "k" };
    if (tempAlias[from] && tempAlias[to]) {
        const tf = tempAlias[from];
        const tt = tempAlias[to];
        let c = tf === "c" ? val : tf === "f" ? (val - 32) * (5 / 9) : val - 273.15;
        let out = tt === "c" ? c : tt === "f" ? c * (9 / 5) + 32 : c + 273.15;
        const unitLbl = tt === "c" ? "°C" : tt === "f" ? "°F" : "K";
        return `${Number(out.toFixed(6)).toLocaleString()} ${unitLbl}`;
    }

    // Data (explicit decimal vs binary)
    if (DATA_UNITS[from] && DATA_UNITS[to]) {
        const bytes = val * DATA_UNITS[from];
        const out = bytes / DATA_UNITS[to];
        return `${Number(out.toFixed(6)).toLocaleString()} ${m[3]}`;
    }

    // Length
    if (LENGTH_UNITS[from] && LENGTH_UNITS[to]) {
        const meters = val * LENGTH_UNITS[from];
        const out = meters / LENGTH_UNITS[to];
        return `${Number(out.toFixed(6)).toLocaleString()} ${m[3]}`;
    }

    // CSS px <-> rem
    if ((from === "px" && to === "rem") || (from === "rem" && to === "px")) {
        const out = from === "px" ? val / remBase : val * remBase;
        return `${Number(out.toFixed(4)).toLocaleString()} ${to} (base ${remBase}px)`;
    }

    return null;
}

export function mount(root, api) {
    let config = { remBase: 16, saveHistory: false, ...api.config };
    let tier = api.tier;
    let history = [];

    root.classList.add("calc");

    // A calculator display: the expression on a quiet line, the result as the hero below it.
    const display = document.createElement("div");
    display.className = "calc-display";

    const inputRow = document.createElement("div");
    inputRow.className = "calc-entry";
    const input = document.createElement("input");
    input.type = "text";
    input.className = "calc-expr mono";
    input.setAttribute("aria-label", "Calculator expression or unit conversion");
    input.spellcheck = false;
    const evalBtn = document.createElement("button");
    evalBtn.type = "button";
    evalBtn.className = "dash-btn primary small calc-eval";
    evalBtn.textContent = "=";
    evalBtn.setAttribute("aria-label", "Evaluate");
    evalBtn.title = "Evaluate";
    inputRow.append(input, evalBtn);

    const outputEl = document.createElement("div");
    outputEl.className = "dash-figure calc-result";
    outputEl.setAttribute("aria-live", "polite");
    outputEl.textContent = "0";

    display.append(inputRow, outputEl);

    const lower = document.createElement("div");
    lower.className = "calc-lower";

    const keypadEl = document.createElement("div");
    keypadEl.className = "calc-keypad";
    // Shown with typographic signs; the field still receives the ASCII operator the parser reads.
    const keyLabels = { "/": "÷", "*": "×", "-": "−", "+": "+" };
    const keys = ["7", "8", "9", "/", "4", "5", "6", "*", "1", "2", "3", "-", "0", ".", "C", "+"];
    for (const k of keys) {
        const b = document.createElement("button");
        b.type = "button";
        b.className = "calc-key";
        if (keyLabels[k]) {
            b.classList.add("is-op");
            b.textContent = keyLabels[k];
            b.setAttribute("aria-label", k === "/" ? "Divide" : k === "*" ? "Multiply" : k === "-" ? "Minus" : "Plus");
        } else if (k === "C") {
            b.classList.add("is-clear");
            b.textContent = k;
            b.setAttribute("aria-label", "Clear");
        } else {
            b.textContent = k;
        }
        b.addEventListener("click", () => {
            if (k === "C") {
                input.value = "";
                showResult("0");
            } else {
                input.value += k;
            }
            input.focus();
        });
        keypadEl.append(b);
    }

    const histEl = document.createElement("ul");
    histEl.className = "dash-list calc-history";

    lower.append(keypadEl, histEl);
    root.append(display, lower);

    function showResult(text, isError = false) {
        outputEl.textContent = text;
        outputEl.title = text;
        outputEl.classList.toggle("is-error", isError);
        outputEl.classList.toggle("is-long", !isError && text.length > 11);
    }

    function compute(recordHistory = false) {
        const raw = input.value.trim();
        if (!raw) {
            showResult("0");
            return;
        }
        const conv = tryUnitConversion(raw, config.remBase || 16);
        if (conv !== null) {
            showResult(conv);
            if (recordHistory)
                pushHistory(raw, conv);
            return;
        }
        try {
            const val = evaluateArithmetic(raw);
            const fmt = Number.isInteger(val) ? val.toLocaleString() : Number(val.toPrecision(12)).toString();
            showResult(fmt);
            if (recordHistory)
                pushHistory(raw, fmt);
        } catch (err) {
            showResult(err.message, true);
        }
    }

    function pushHistory(expr, result) {
        history.unshift({ expr, result });
        if (history.length > 8)
            history.pop();
        renderHistory();
    }

    function renderHistory() {
        histEl.replaceChildren();
        if (tier === "compact")
            return;
        if (history.length === 0) {
            const empty = document.createElement("li");
            empty.className = "calc-history-empty";
            empty.textContent = "Press = or Enter to keep a result here";
            histEl.append(empty);
            return;
        }
        for (const h of history.slice(0, 4)) {
            const li = document.createElement("li");
            li.className = "dash-list-item";
            const btn = document.createElement("button");
            btn.type = "button";
            btn.className = "dash-link-btn calc-history-btn mono tabular";
            btn.title = `${h.expr} = ${h.result}`;
            const exprSpan = document.createElement("span");
            exprSpan.className = "dash-truncate calc-history-expr";
            exprSpan.textContent = h.expr;
            const resSpan = document.createElement("span");
            resSpan.className = "dash-truncate calc-history-result";
            resSpan.textContent = `= ${h.result}`;
            btn.append(exprSpan, resSpan);
            btn.addEventListener("click", () => {
                input.value = h.expr;
                compute(false);
                input.focus();
            });
            li.append(btn);
            histEl.append(li);
        }
    }

    function render() {
        input.placeholder = tier === "compact" ? "(12+8)*3" : "(12+8)*3 · 32 F to C · 1 MiB to B";
        // The narrow tile's eyebrow has no room for it; the value stays in Settings.
        api.setMeta(tier === "compact" ? "" : `rem=${config.remBase || 16}px`);
        keypadEl.classList.toggle("hidden", tier !== "expanded");
        renderHistory();
        lower.classList.toggle("hidden", tier === "compact");
    }

    evalBtn.addEventListener("click", () => compute(true));
    input.addEventListener("keydown", e => {
        if (e.key === "Enter") {
            e.preventDefault();
            compute(true);
        }
    });
    input.addEventListener("input", () => compute(false));

    render();

    return {
        onUpdate(next) {
            config = { remBase: 16, saveHistory: false, ...next.config };
            tier = next.tier;
            render();
        },
        renderConfig(container, close) {
            const lbl = document.createElement("label");
            lbl.textContent = "Root font size for rem conversions (px)";
            const inp = document.createElement("input");
            inp.type = "number";
            inp.className = "dash-input";
            inp.min = "6";
            inp.max = "64";
            inp.value = String(config.remBase || 16);
            const foot = document.createElement("div");
            foot.className = "dialog-footer";
            const save = document.createElement("button");
            save.type = "button";
            save.className = "primary-button";
            save.textContent = "Save";
            save.addEventListener("click", async () => {
                const v = Math.max(6, Math.min(64, Number.parseInt(inp.value, 10) || 16));
                await api.updateConfig({ remBase: v });
                close();
            });
            foot.append(save);
            container.append(lbl, inp, foot);
        },
        unmount() {},
    };
}
