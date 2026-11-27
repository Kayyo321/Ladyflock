/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #30 Pinned References and Browser Tools (pinned-references)
 * Safe built-in tool list (omitting profile-wide pages like about:bookmarks in private mode)
 * + opaque user-picked local document references in persistent default scope.
 */

import { icon } from "../icons.js";

// Glyphs for the built-in tools, by page. Anything unknown falls back to its initial.
const TOOL_GLYPHS = {
    "about:settings": "configure",
    "about:privacy": "privacy-summary",
    "about:bookmarks": "bookmark-shelf",
    "about:history": "recent-history",
    "about:downloads": "downloads",
    "about:addons": "extensions-dock",
    "about:support": "browser-health",
    "about:version": "workspaces",
};

export function mount(root, api) {
    let tier = api.tier;
    let state = { tools: [], files: [], canPinLocalFiles: false };

    root.classList.add("refs");

    const toolsWrap = document.createElement("div");
    toolsWrap.className = "refs-tools";

    const filesHead = document.createElement("div");
    filesHead.className = "refs-files-head";
    const filesTitle = document.createElement("span");
    filesTitle.className = "dash-label";
    filesTitle.textContent = "Pinned documents";
    const pickBtn = document.createElement("button");
    pickBtn.type = "button";
    pickBtn.className = "dash-btn ghost small refs-pick";
    pickBtn.append(icon("plus", 13));
    const pickLbl = document.createElement("span");
    pickLbl.textContent = "Pin file…";
    pickBtn.append(pickLbl);
    pickBtn.addEventListener("click", async () => {
        try {
            const res = await api.request("references.pickFile", {});
            if (!res.canceled)
                await load();
        } catch (err) {
            api.announce(err.message || "Couldn’t pin file.");
        }
    });
    filesHead.append(filesTitle, pickBtn);

    const filesList = document.createElement("ul");
    filesList.className = "dash-list refs-files";

    root.append(toolsWrap, filesHead, filesList);

    function render() {
        toolsWrap.replaceChildren();
        const capTools = tier === "compact" ? 4 : state.tools.length;
        for (const t of (state.tools || []).slice(0, capTools)) {
            const b = document.createElement("button");
            b.type = "button";
            b.className = "refs-tool";
            b.title = t.description ? `${t.label} — ${t.description}` : t.label;
            b.setAttribute("aria-label", t.label);
            const tile = document.createElement("span");
            tile.className = "refs-tool-icon";
            const glyph = TOOL_GLYPHS[t.url];
            if (glyph)
                tile.append(icon(glyph, 19));
            else
                tile.textContent = (t.label || "?").slice(0, 1).toUpperCase();
            const label = document.createElement("span");
            label.className = "refs-tool-label dash-truncate";
            label.textContent = t.label;
            b.append(tile, label);
            b.addEventListener("click", () => api.openUrl(t.url, false));
            toolsWrap.append(b);
        }

        const showFiles = state.canPinLocalFiles && tier !== "compact";
        root.classList.toggle("has-files", showFiles);
        filesHead.classList.toggle("hidden", !showFiles);
        filesList.classList.toggle("hidden", !showFiles);
        api.setMeta(`${state.tools.length} tools`);

        filesList.replaceChildren();
        if (showFiles) {
            if ((state.files || []).length === 0) {
                const empty = document.createElement("li");
                empty.className = "refs-empty";
                empty.textContent = "Pin local PDFs, HTML, or text files for quick reference.";
                filesList.append(empty);
            } else {
                for (const f of state.files) {
                    const li = document.createElement("li");
                    li.className = "dash-list-item refs-file";
                    li.classList.toggle("is-missing", Boolean(f.missing));

                    const openBtn = document.createElement("button");
                    openBtn.type = "button";
                    openBtn.className = "dash-link-btn";
                    const glyph = document.createElement("span");
                    glyph.className = "refs-file-glyph";
                    glyph.append(icon("scratchpad", 15));
                    const name = document.createElement("span");
                    name.className = "dash-truncate refs-file-name";
                    name.textContent = f.label || f.basename;
                    openBtn.append(glyph, name);
                    if (f.missing) {
                        const warn = document.createElement("span");
                        warn.className = "dash-badge warn";
                        warn.textContent = "Missing";
                        openBtn.append(warn);
                    }
                    openBtn.addEventListener("click", async () => {
                        try {
                            await api.request("references.openFile", { id: f.id });
                        } catch (err) {
                            api.announce(err.message || "File missing; please reselect.");
                            await load();
                        }
                    });

                    const actions = document.createElement("span");
                    actions.className = "dash-row-actions";
                    const rm = document.createElement("button");
                    rm.type = "button";
                    rm.className = "dash-btn ghost icon small";
                    rm.setAttribute("aria-label", `Unpin ${f.label || f.basename}`);
                    rm.title = "Unpin";
                    rm.append(icon("close", 13));
                    rm.addEventListener("click", async () => {
                        await api.request("references.removeFile", { id: f.id });
                        await load();
                    });
                    actions.append(rm);

                    li.append(openBtn, actions);
                    filesList.append(li);
                }
            }
        }
    }

    async function load() {
        state = await api.request("references.list", {});
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
