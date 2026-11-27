/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #22 Now Playing (now-playing)
 * Scoped strictly to views in the caller's BrowsingSession via state_for_view / handle_action_for_view.
 */

import { icon } from "../icons.js";

function fmtTime(sec) {
    if (!Number.isFinite(sec) || sec < 0)
        return "0:00";
    const s = Math.floor(sec);
    const h = Math.floor(s / 3600);
    const m = Math.floor((s % 3600) / 60);
    const r = String(s % 60).padStart(2, "0");
    return h > 0 ? `${h}:${String(m).padStart(2, "0")}:${r}` : `${m}:${r}`;
}

export function mount(root, api) {
    let tier = api.tier;
    let sessions = [];
    let selectedViewId = null;
    let pollTimer = null;

    root.classList.add("np");

    const wrap = document.createElement("div");
    wrap.className = "np-face";
    root.append(wrap);

    function render() {
        // The face is rebuilt on every poll; keep keyboard focus on the same control.
        const focusKey = wrap.contains(document.activeElement) ? document.activeElement.dataset.focusKey : null;
        wrap.replaceChildren();
        renderFace();
        if (focusKey) {
            const again = wrap.querySelector(`[data-focus-key="${focusKey}"]`);
            if (again)
                again.focus();
        }
    }

    function renderFace() {
        if (!sessions.length) {
            api.setMeta("Idle");
            wrap.classList.add("is-idle");
            const art = document.createElement("span");
            art.className = "np-art";
            art.append(icon("now-playing", tier === "expanded" ? 22 : 16));
            const empty = document.createElement("span");
            empty.className = "np-idle-text";
            empty.textContent = "No media playing in this session.";
            wrap.append(art, empty);
            return;
        }
        wrap.classList.remove("is-idle");

        let active = sessions.find(s => s.viewId === selectedViewId) || sessions[0];
        selectedViewId = active.viewId;
        api.setMeta(active.playing ? "Playing" : "Paused");

        const top = document.createElement("div");
        top.className = "np-main";

        if (tier === "expanded") {
            const art = document.createElement("span");
            art.className = "np-art";
            art.append(icon("now-playing", 22));
            top.append(art);
        }

        const info = document.createElement("div");
        info.className = "np-info";
        const title = document.createElement("span");
        title.className = "dash-truncate np-title";
        title.textContent = active.title || active.tabTitle || "Media";
        title.title = title.textContent;
        info.append(title);
        // Where it is playing: the artist if the page says, otherwise the tab.
        const source = active.artist || (active.title && active.tabTitle !== active.title ? active.tabTitle : "");
        if (source) {
            const sourceEl = document.createElement("span");
            sourceEl.className = "dash-muted dash-truncate np-source";
            sourceEl.textContent = source;
            info.append(sourceEl);
        }

        const controls = document.createElement("div");
        controls.className = "np-controls";
        const supported = new Set(active.supportedActions || []);

        const sendAct = async (action, seekTime = null) => {
            try {
                await api.request("media.action", { viewId: active.viewId, action, seekTime });
            } finally {
                setTimeout(() => load(), 120);
            }
        };

        const transport = (glyph, label, key, action, extra = "") => {
            const b = document.createElement("button");
            b.type = "button";
            b.className = `np-btn ${extra}`.trim();
            b.dataset.focusKey = key;
            b.setAttribute("aria-label", label);
            b.title = label;
            b.append(icon(glyph, extra ? 18 : 16));
            b.addEventListener("click", () => sendAct(action));
            return b;
        };

        if (tier !== "compact" && supported.has("previoustrack"))
            controls.append(transport("prev", "Previous track", "prev", "previoustrack"));

        const ppBtn = transport(
            active.playing ? "pause" : "play",
            active.playing ? "Pause" : "Play",
            "playpause",
            active.playing ? "pause" : "play",
            "np-btn-main");
        controls.append(ppBtn);

        if (tier !== "compact" && supported.has("nexttrack"))
            controls.append(transport("next", "Next track", "next", "nexttrack"));

        top.append(info, controls);
        wrap.append(top);

        if (tier === "expanded" && active.position && active.position.duration > 0 && supported.has("seekto")) {
            const seekRow = document.createElement("div");
            seekRow.className = "np-seek-row";
            const pos = document.createElement("span");
            pos.className = "np-time tabular";
            pos.textContent = fmtTime(active.position.position || 0);
            const seek = document.createElement("input");
            seek.type = "range";
            seek.className = "np-seek";
            seek.dataset.focusKey = "seek";
            seek.min = "0";
            seek.max = String(Math.floor(active.position.duration));
            seek.value = String(Math.floor(active.position.position || 0));
            seek.setAttribute("aria-label", "Seek position");
            seek.setAttribute("aria-valuetext", `${fmtTime(active.position.position || 0)} of ${fmtTime(active.position.duration)}`);
            seek.addEventListener("change", () => sendAct("seekto", Number(seek.value)));
            const dur = document.createElement("span");
            dur.className = "np-time tabular";
            dur.textContent = fmtTime(active.position.duration);
            seekRow.append(pos, seek, dur);
            wrap.append(seekRow);
        }

        if (tier === "expanded" && sessions.length > 1) {
            const sel = document.createElement("select");
            sel.className = "dash-select np-source-select";
            sel.dataset.focusKey = "source";
            sel.setAttribute("aria-label", "Select media source tab");
            for (const s of sessions) {
                const opt = document.createElement("option");
                opt.value = String(s.viewId);
                opt.textContent = s.title || s.tabTitle || "Media tab";
                sel.append(opt);
            }
            sel.value = String(active.viewId);
            sel.addEventListener("change", () => {
                selectedViewId = Number(sel.value);
                render();
            });
            wrap.append(sel);
        }
    }

    async function load() {
        const res = await api.request("media.list", {});
        sessions = res.sessions || [];
        render();
    }

    const onVis = () => {
        if (document.hidden)
            clearInterval(pollTimer);
        else {
            load().catch(() => {});
            pollTimer = setInterval(() => load().catch(() => {}), 3000);
        }
    };
    document.addEventListener("visibilitychange", onVis);

    load().catch(err => api.reportError(err.message));
    pollTimer = setInterval(() => {
        if (!document.hidden)
            load().catch(() => {});
    }, 3000);

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {
            clearInterval(pollTimer);
            document.removeEventListener("visibilitychange", onVis);
        },
    };
}
