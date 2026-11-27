/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * #14 Profile and Identity (profile-switcher)
 * Identifies current profile and browsing identity separately without enumerating regular profiles in private mode.
 */

import { icon } from "../icons.js";

function el(tag, className, text) {
    const node = document.createElement(tag);
    if (className)
        node.className = className;
    if (text !== undefined)
        node.textContent = text;
    return node;
}

function initialOf(text) {
    const ch = String(text || "").trim().slice(0, 1);
    return ch ? ch.toUpperCase() : "?";
}

export function mount(root, api) {
    let tier = api.tier;
    let info = null;
    root.classList.add("prof");

    // One line of the face: a mark, a name, and a small caption underneath.
    function line(mark, label, name, caption) {
        const row = el("div", "prof-line");
        row.append(mark);
        const text = el("div", "prof-text");
        text.append(el("span", "dash-label", label), el("span", "prof-name dash-truncate", name));
        if (caption)
            text.append(el("span", "dash-muted dash-truncate", caption));
        row.append(text);
        return row;
    }

    function render() {
        root.replaceChildren();
        if (!info || !info.available) {
            api.setMeta("Unavailable");
            root.append(el("div", "dash-empty-card", "Profile details are unavailable."));
            return;
        }

        const id = info.currentIdentity || {};
        api.setMeta(info.isPrivate ? "Private" : (id.kind || "Standard"));

        const identityName = info.isPrivate ? "Private browsing" : (id.name || "Default identity");
        const profileName = info.isPrivate ? "Hidden in private windows" : (info.profileIdentifier || "default");
        const persistence = info.isEphemeral ? "Ephemeral" : "Persistent";

        if (tier === "compact") {
            const avatar = el("span", "prof-avatar", initialOf(identityName));
            avatar.setAttribute("aria-hidden", "true");
            const row = el("div", "prof-compact");
            const text = el("div", "prof-text");
            text.append(
                el("span", "prof-name dash-truncate", identityName),
                el("span", "dash-muted dash-truncate", persistence)
            );
            row.append(avatar, text);
            root.append(row);
            return;
        }

        const profileMark = el("span", "prof-avatar", initialOf(info.isPrivate ? "P" : profileName));
        profileMark.setAttribute("aria-hidden", "true");
        const identityMark = el("span", "prof-avatar is-identity");
        identityMark.append(icon("profile-switcher", 15));

        const profileCaption = info.isPrivate
            ? "Isolated private session"
            : `${info.profileIsTemporary ? "Temporary · " : ""}History ${info.persistsHistory ? "on" : "off"}`;
        const kind = id.kind && id.kind.toLowerCase() !== persistence.toLowerCase()
            ? id.kind.charAt(0).toUpperCase() + id.kind.slice(1)
            : "";
        const lines = el("div", "prof-lines");
        lines.append(
            line(profileMark, "Profile", profileName, profileCaption),
            line(identityMark, "Identity", identityName, `${kind ? kind + " · " : ""}${persistence}`)
        );
        root.append(lines);

        const privBtn = el("button", "dash-btn small prof-private");
        privBtn.type = "button";
        privBtn.append(icon("private", 13), document.createTextNode("New private window"));
        privBtn.addEventListener("click", () => {
            api.request("profile.openPrivateWindow", {}).catch(() => {});
        });
        root.append(privBtn);

        if (tier === "expanded" && !info.isPrivate && Array.isArray(info.identities) && info.identities.length > 0) {
            const ul = el("ul", "dash-list prof-identities");
            ul.setAttribute("aria-label", `Configured identities (${info.identities.length})`);
            for (const item of info.identities) {
                const li = el("li", "dash-row-between prof-identity");
                li.append(el("span", "dash-truncate", item.name || item.id), el("span", "dash-muted", item.kind));
                ul.append(li);
            }
            root.append(ul);
        }
    }

    api.request("profile.summary", {})
        .then(res => {
            info = res;
            render();
        })
        .catch(err => api.reportError(err.message));

    return {
        onUpdate(next) {
            tier = next.tier;
            render();
        },
        unmount() {},
    };
}
