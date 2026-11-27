/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #17 Focus Timer (focus-timer)
 * Focus countdown, break countdown, stopwatch, and local completion log.
 * Persists transitions only (never per-second writes); reconciles elapsed run once via runId.
 */

import { icon, ring } from "../icons.js";

const PRESETS = [
    { id: "focus", label: "Focus", seconds: 25 * 60 },
    { id: "break", label: "Break", seconds: 5 * 60 },
    { id: "stopwatch", label: "Stopwatch", seconds: 0 },
];

function fmtDuration(sec) {
    const s = Math.max(0, Math.floor(sec));
    const m = Math.floor(s / 60);
    const r = s % 60;
    return `${String(m).padStart(2, "0")}:${String(r).padStart(2, "0")}`;
}

export function mount(root, api) {
    let tier = api.tier;
    let rev = 0;
    let state = {
        mode: "focus",
        running: false,
        runId: "",
        durationSec: 25 * 60,
        remainingSec: 25 * 60,
        deadlineMs: 0,
        startedAtMs: 0,
        elapsedSec: 0,
        completedRunIds: [],
        log: [],
    };
    let tickTimer = null;

    root.classList.add("timer");

    const modeRow = document.createElement("div");
    modeRow.className = "dash-segments timer-modes";
    modeRow.setAttribute("role", "group");
    modeRow.setAttribute("aria-label", "Timer mode");
    const modeButtons = new Map();
    for (const p of PRESETS) {
        const b = document.createElement("button");
        b.type = "button";
        b.className = "dash-btn";
        b.textContent = p.label;
        b.addEventListener("click", () => switchMode(p));
        modeButtons.set(p.id, b);
        modeRow.append(b);
    }

    // The dial: a ring that empties as the countdown runs, with the time inside it.
    const dialEl = document.createElement("div");
    dialEl.className = "timer-dial";
    let dialRing = null;
    let dialSize = 0;

    const displayEl = document.createElement("div");
    displayEl.className = "timer-time tabular";
    dialEl.append(displayEl);

    const ctrlRow = document.createElement("div");
    ctrlRow.className = "timer-controls";
    const startPauseBtn = document.createElement("button");
    startPauseBtn.type = "button";
    startPauseBtn.className = "dash-btn primary timer-start";
    const startPauseLabel = document.createElement("span");
    const resetBtn = document.createElement("button");
    resetBtn.type = "button";
    resetBtn.className = "dash-btn ghost icon";
    resetBtn.setAttribute("aria-label", "Reset");
    resetBtn.title = "Reset";
    resetBtn.append(icon("reset", 15));
    ctrlRow.append(startPauseBtn, resetBtn);

    const mainRow = document.createElement("div");
    mainRow.className = "timer-main";
    mainRow.append(dialEl, ctrlRow);

    const logEl = document.createElement("div");
    logEl.className = "dash-muted timer-log";

    root.append(modeRow, mainRow, logEl);

    function ensureDial() {
        const size = tier === "compact" ? 0 : tier === "expanded" ? 132 : 92;
        if (size === dialSize)
            return;
        dialSize = size;
        dialRing?.el.remove();
        dialRing = null;
        if (size) {
            dialRing = ring(size, tier === "expanded" ? 8 : 7);
            dialEl.prepend(dialRing.el);
        }
    }

    function updateDial(secs) {
        if (!dialRing)
            return;
        if (state.mode === "stopwatch")
            dialRing.set((secs % 60) / 60);
        else
            dialRing.set(state.durationSec > 0 ? secs / state.durationSec : 0);
    }

    function currentSeconds() {
        if (state.mode === "stopwatch") {
            if (state.running && state.startedAtMs > 0)
                return state.elapsedSec + (Date.now() - state.startedAtMs) / 1000;
            return state.elapsedSec;
        }
        if (state.running && state.deadlineMs > 0)
            return Math.max(0, (state.deadlineMs - Date.now()) / 1000);
        return state.remainingSec;
    }

    async function persist(nextState) {
        try {
            const res = await api.request("items.save", {
                collection: "focus-timer",
                itemId: "state",
                expectedRevision: rev,
                value: nextState,
            });
            rev = res.revision;
            state = nextState;
            render();
        } catch (err) {
            if (err.conflict)
                await load();
        }
    }

    async function reconcileCompletionIfNeeded() {
        if (state.mode === "stopwatch" || !state.running || !state.deadlineMs)
            return;
        if (Date.now() >= state.deadlineMs) {
            const alreadyLogged = (state.completedRunIds || []).includes(state.runId);
            const nextLog = alreadyLogged
                ? state.log
                : [{ mode: state.mode, durationSec: state.durationSec, completedAt: Date.now() }, ...(state.log || [])].slice(0, 20);
            const nextIds = alreadyLogged
                ? state.completedRunIds
                : [state.runId, ...(state.completedRunIds || [])].slice(0, 30);
            await persist({
                ...state,
                running: false,
                remainingSec: 0,
                deadlineMs: 0,
                completedRunIds: nextIds,
                log: nextLog,
            });
            api.announce(`${state.mode === "focus" ? "Focus" : "Break"} session complete.`);
        }
    }

    function switchMode(preset) {
        persist({
            ...state,
            mode: preset.id,
            running: false,
            runId: "",
            durationSec: preset.seconds,
            remainingSec: preset.seconds,
            deadlineMs: 0,
            startedAtMs: 0,
            elapsedSec: 0,
        });
    }

    startPauseBtn.addEventListener("click", () => {
        if (state.running) {
            if (state.mode === "stopwatch") {
                persist({
                    ...state,
                    running: false,
                    elapsedSec: currentSeconds(),
                    startedAtMs: 0,
                });
            } else {
                persist({
                    ...state,
                    running: false,
                    remainingSec: currentSeconds(),
                    deadlineMs: 0,
                });
            }
        } else {
            const runId = state.runId || `run-${Date.now().toString(36)}-${Math.random().toString(36).slice(2, 6)}`;
            if (state.mode === "stopwatch") {
                persist({
                    ...state,
                    running: true,
                    runId,
                    startedAtMs: Date.now(),
                });
            } else {
                const rem = state.remainingSec > 0 ? state.remainingSec : state.durationSec;
                persist({
                    ...state,
                    running: true,
                    runId,
                    remainingSec: rem,
                    deadlineMs: Date.now() + rem * 1000,
                });
            }
        }
    });

    resetBtn.addEventListener("click", () => {
        const p = PRESETS.find(x => x.id === state.mode) || PRESETS[0];
        switchMode(p);
    });

    function render() {
        ensureDial();
        modeRow.classList.toggle("hidden", tier === "compact");
        for (const [id, btn] of modeButtons)
            btn.setAttribute("aria-pressed", state.mode === id ? "true" : "false");
        const secs = currentSeconds();
        displayEl.textContent = fmtDuration(secs);
        updateDial(secs);
        startPauseLabel.textContent = state.running ? "Pause" : "Start";
        startPauseBtn.replaceChildren(icon(state.running ? "pause" : "play", 13), startPauseLabel);
        startPauseBtn.setAttribute("aria-label", state.running ? "Pause" : "Start");
        root.classList.toggle("is-running", Boolean(state.running));
        const modeLabel = PRESETS.find(p => p.id === state.mode)?.label || "";
        const status = state.running ? "Running" : (state.remainingSec === 0 && state.mode !== "stopwatch" ? "Done" : "Ready");
        api.setMeta(tier === "compact" ? `${modeLabel} · ${status}` : status);

        const completedCount = (state.log || []).length;
        logEl.classList.toggle("hidden", tier !== "expanded");
        logEl.textContent = completedCount > 0
            ? `${completedCount} ${completedCount === 1 ? "session" : "sessions"} completed`
            : "No sessions completed yet";

        clearInterval(tickTimer);
        if (state.running && !document.hidden) {
            tickTimer = setInterval(() => {
                const now = currentSeconds();
                displayEl.textContent = fmtDuration(now);
                updateDial(now);
                reconcileCompletionIfNeeded();
            }, 500);
        }
    }

    async function load() {
        const res = await api.request("items.list", { collection: "focus-timer", limit: 5 });
        const found = (res.items || []).find(it => it.itemId === "state");
        if (found) {
            rev = found.revision;
            state = { ...state, ...found.value };
        }
        render();
        await reconcileCompletionIfNeeded();
    }

    const offUp = api.onEvent("itemUpdated", ev => {
        if (ev.collection === "focus-timer" && ev.itemId === "state") {
            rev = ev.revision;
            state = { ...state, ...ev.value };
            render();
        }
    });
    const onVis = () => {
        if (!document.hidden) {
            reconcileCompletionIfNeeded();
            render();
        } else {
            clearInterval(tickTimer);
        }
    };
    document.addEventListener("visibilitychange", onVis);

    load().catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            clearInterval(tickTimer);
            offUp();
            document.removeEventListener("visibilitychange", onVis);
        },
    };
}
