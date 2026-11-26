/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

const byId = id => document.getElementById(id);
const review = byId("review");
const installed = byId("installed");
const query = byId("query");
const catalogStatus = byId("catalog-status");
const notice = byId("notice");
const reviewActionLabel = "Review/Install Extension";

let extensions = [];
let operation = null;
let confirmation = null;
let searchId = 0;
let searchQuery = "";
let searchPage = 1;
let catalogEntries = [];
let catalogAvailable = false;

function element(tag, text, className) {
    const node = document.createElement(tag);
    if (text !== undefined) node.textContent = text;
    if (className) node.className = className;
    return node;
}

const iconRequests = new Map();

function extensionIdentity(item, label) {
    const identity = element("span", undefined, "extension-identity");
    const icon = byId("extension-icon-template").content.firstElementChild.cloneNode(true);
    identity.append(icon, element("span", label));
    if (item.icon_key) {
        icon.dataset.iconKey = item.icon_key;
        if (!iconRequests.has(item.icon_key)) {
            iconRequests.set(item.icon_key, null);
            ladybird.sendMessage("getExtensionIcon", { icon_key: item.icon_key });
        }
        showExtensionIcon(icon, iconRequests.get(item.icon_key));
    }
    return identity;
}

function showExtensionIcon(icon, url) {
    if (!url) return;
    const image = element("img");
    image.alt = "";
    image.addEventListener("load", () => icon.replaceChildren(image), { once: true });
    image.src = url;
}

function receivedIcon(data) {
    if (!iconRequests.has(data.icon_key)) return;
    const url = data.icon_data_url;
    if (typeof url !== "string" || !url.startsWith("data:image/png;base64,")) return;
    iconRequests.set(data.icon_key, url);
    for (const icon of document.querySelectorAll(".extension-icon[data-icon-key]")) {
        if (icon.dataset.iconKey === data.icon_key) showExtensionIcon(icon, url);
    }
}

function button(text, onClick, className = "secondary-button") {
    const node = element("button", text, className);
    node.type = "button";
    node.addEventListener("click", onClick);
    return node;
}

function announce(node, message, error = false) {
    node.textContent = message;
    node.classList.toggle("error", error);
}

function setCatalogReviewBusy(stage, busy) {
    stage.textContent = busy ? "Preparing review…" : reviewActionLabel;
    stage.ariaBusy = busy ? "true" : null;
    stage.ariaDisabled = busy ? "true" : null;
}

function sendAction(name, data, status, trigger) {
    if (operation) return false;
    operation = { name, trigger, extensionId: data.extension_id };
    if (name === "stageCatalogInstall") setCatalogReviewBusy(trigger, true);
    announce(notice, status);
    ladybird.sendMessage(name, data);
    return true;
}

function finishAction(message, error = false) {
    operation = null;
    announce(notice, message, error);
}

function textList(values) {
    const list = element("ul");
    for (const value of values) list.append(element("li", value));
    return list;
}

const permissionDescriptions = new Map([
    ["activeTab", "Access the current tab when you invoke the extension"],
    ["alarms", "Schedule extension tasks to run later or repeat"],
    ["contextMenus", "Add commands to webpage context menus"],
    ["declarativeNetRequest", "Apply extension rules to network requests"],
    ["dns", "Look up network addresses for hostnames"],
    ["menus", "Add commands to webpage context menus"],
    ["privacy", "Read and change the browser’s network prediction setting"],
    ["publicSuffix", "Find the public suffix and registrable domain of a hostname"],
    ["scripting", "Run scripts and change styles on permitted sites"],
    ["storage", "Save extension data in this profile"],
    ["tabs", "Read information about your tabs"],
    ["unlimitedStorage", "Save more extension data than the usual local storage limit"],
    ["webNavigation", "Read information about page and frame navigation in your tabs"],
    ["webRequest", "Observe requests to permitted sites"],
    ["webRequestBlocking", "Block requests to permitted sites"],
    ["<all_urls>", "All requested web addresses"],
]);

function describePermission(value) {
    const description = permissionDescriptions.get(value);
    return description ? description + " (" + value + ")" : value;
}

function group(container, heading, values) {
    if (!values?.length) return;
    const section = element("section", undefined, "permission-group");
    section.append(element("h3", heading), textList(values.map(describePermission)));
    container.append(section);
}

function compatibility(entry) {
    if (!entry.compatibility_status || entry.compatibility_status === "unverified") {
        return "Compatibility pending package review";
    }
    return entry.compatibility_status;
}

function optionSwitch(title, checked, onChange) {
    const label = element("label", undefined, "switch-label");
    const input = element("input");
    input.type = "checkbox";
    input.setAttribute("switch", "");
    input.checked = checked;
    input.addEventListener("change", () => {
        // The browser owns this setting. Keep the current value until it confirms the change.
        const requested = input.checked;
        input.checked = checked;
        onChange(requested, input);
    });
    label.append(input, document.createTextNode(title));
    return label;
}

function confirmAction(title, content, action, accept, cancel, trigger) {
    confirmation = { accept, cancel, trigger };
    byId("review-title").replaceChildren(title);
    byId("review-body").replaceChildren(content);
    byId("confirm").textContent = action;
    review.showModal();
    byId("cancel").focus();
}

function closeReview(approved) {
    if (!confirmation) return;
    const { accept, cancel, trigger } = confirmation;
    confirmation = null;
    review.close();
    if (trigger?.isConnected) trigger.focus();
    if (approved) accept();
    else cancel();
}

byId("cancel").addEventListener("click", () => closeReview(false));
byId("confirm").addEventListener("click", () => closeReview(true));
review.addEventListener("cancel", event => {
    event.preventDefault();
    closeReview(false);
});
review.addEventListener("keydown", event => {
    if (event.key === "Escape") {
        event.preventDefault();
        closeReview(false);
    }
});

function installedEntry(ext) {
    const entry = element("article", undefined, "entry");
    entry.dataset.extension = ext.extension_id;
    const id = { extension_id: ext.extension_id };
    const header = element("div", undefined, "entry-header");
    const title = element("h3");
    title.append(extensionIdentity(ext, ext.name));
    title.dir = "auto";
    const enabled = optionSwitch(ext.enabled ? "Enabled" : "Disabled", ext.enabled, (value, input) => {
        sendAction("setExtensionEnabled", { ...id, enabled: value }, "Saving extension settings…", input);
    });
    enabled.firstChild.setAttribute("aria-label", "Enable " + ext.name);
    enabled.firstChild.dataset.control = "enabled";
    header.append(title, enabled);
    const source =
        ext.source === "unpacked" ? "Unpacked extension" : ext.verified_signature ? "Verified signature" : "";
    entry.append(
        header,
        element("p", ["Version " + ext.version, source, compatibility(ext)].filter(Boolean).join(" · "), "description")
    );

    const access = element("details");
    const summary = element("summary", "Site access");
    summary.dataset.control = "access";
    access.append(summary);
    const allSites = optionSwitch("Allow requested sites by default", ext.allow_all_sites, (value, input) => {
        sendAction("setExtensionSiteAccess", { ...id, allow_all_sites: value }, "Saving site access…", input);
    });
    allSites.firstChild.dataset.control = "all-sites";
    access.append(allSites);
    if (ext.host_permissions?.length) {
        access.append(element("p", "Requested sites", "description"), textList(ext.host_permissions));
    }
    if (ext.host_permissions?.length && Array.isArray(ext.granted_hosts) && Array.isArray(ext.revoked_hosts)) {
        const exceptions = element("form", undefined, "controls");
        const field = element("input");
        field.type = "text";
        field.placeholder = "example.com";
        field.setAttribute("aria-label", "Site hostname");
        field.required = true;
        const choice = element("select");
        choice.setAttribute("aria-label", "Site access for this host");
        choice.add(new Option("Allow", "allow"));
        choice.add(new Option("Block", "block"));
        const save = element("button", "Save Site", "secondary-button");
        exceptions.append(field, choice, save);
        exceptions.addEventListener("submit", event => {
            event.preventDefault();
            const host = field.value.trim();
            if (!host || /[\s\/?#@\\]/.test(host) || !URL.canParse("https://" + host)) {
                announce(notice, "Enter a hostname such as example.com.", true);
                field.focus();
                return;
            }
            const url = new URL("https://" + host);
            if (url.port || (host.includes(":") && !host.endsWith("]"))) {
                announce(notice, "Enter a hostname without a port.", true);
                field.focus();
                return;
            }
            sendAction(
                "setExtensionSiteAccess",
                {
                    ...id,
                    allow_all_sites: ext.allow_all_sites,
                    host: url.hostname,
                    grant: choice.value === "allow",
                },
                "Saving site access…",
                save
            );
        });
        save.dataset.control = "save-site";
        access.append(
            element("p", "Site exceptions apply within the requested sites above.", "description"),
            exceptions
        );
    }
    if (ext.granted_hosts?.length) {
        access.append(element("p", "Allowed sites", "description"), textList(ext.granted_hosts));
    }
    if (ext.revoked_hosts?.length) {
        access.append(element("p", "Blocked sites", "description"), textList(ext.revoked_hosts));
    }
    entry.append(access);

    const actions = element("div", undefined, "controls");
    if (ext.has_options && ext.enabled) {
        const options = button("Options", event => {
            sendAction("openExtensionOptions", id, "Opening extension options…", event.currentTarget);
        });
        options.dataset.control = "options";
        actions.append(options);
    }
    if (ext.can_check_update) {
        const update = button("Check for Update", event => {
            sendAction("checkManualUpdate", id, "Checking for an update…", event.currentTarget);
        });
        update.dataset.control = "update";
        actions.append(update);
    } else if (ext.source === "signed_xpi") {
        const update = button("Update from File...", event => {
            sendAction("pickLocalXpi", id, "Choose a signed XPI update to review.", event.currentTarget);
        });
        update.dataset.control = "update-file";
        actions.append(update);
    }
    const remove = button("Remove...", event => {
        if (operation) return;
        const trigger = event.currentTarget;
        confirmAction(
            extensionIdentity(ext, "Remove " + ext.name + "?"),
            element("p", "This removes the extension and its saved data from this profile."),
            "Remove",
            () => sendAction("removeExtension", id, "Removing extension…", trigger),
            () => {},
            trigger
        );
    });
    remove.dataset.control = "remove";
    actions.append(remove);
    entry.append(actions);
    return entry;
}

function renderInstalled(data) {
    const focused = document.activeElement;
    const focusedId = focused?.closest("[data-extension]")?.dataset.extension;
    const focusedControl = focused?.dataset.control;
    const expanded = new Set(
        Array.from(installed.querySelectorAll("details[open]"), node => node.parentNode.dataset.extension)
    );
    installed.replaceChildren();
    extensions = data.extensions || [];
    if (data.extensions_enabled === false) {
        installed.append(element("p", "Extension execution is turned off for this session.", "description"));
    }
    if (extensions.length === 0 && data.extensions_enabled !== false) {
        installed.append(element("p", "No extensions installed.", "description"));
    }
    extensions.sort((a, b) => a.name.localeCompare(b.name));
    for (const ext of extensions) {
        const entry = installedEntry(ext);
        entry.querySelector("details").open = expanded.has(ext.extension_id);
        installed.append(entry);
    }
    if (focusedId && focusedControl) {
        const entry = Array.from(installed.children).find(node => node.dataset.extension === focusedId);
        const control = Array.from(entry?.querySelectorAll("[data-control]") || []).find(
            node => node.dataset.control === focusedControl
        );
        (control || byId("installed-heading")).focus();
    }
    byId("pick-file").disabled = !Array.isArray(data.extensions) || data.extensions_enabled === false;
    catalogAvailable = data.enable_public_amo_catalog === true;
    query.disabled = byId("search").disabled = !catalogAvailable;
    if (!catalogAvailable) {
        catalogEntries = [];
        byId("pages").hidden = true;
        announce(catalogStatus, "To search Mozilla Add-ons, launch Ladyflock with --enable-amo-catalog.");
    }
    renderResults();
}

function renderResults() {
    const results = byId("results");
    const focusedEntry = document.activeElement?.closest("[data-addon]")?.dataset.addon;
    results.replaceChildren();
    for (const ext of catalogEntries) {
        const entry = element("article", undefined, "entry");
        entry.dataset.addon = ext.addon_id;
        const title = element("h3");
        title.append(extensionIdentity(ext, ext.name));
        title.tabIndex = -1;
        title.dir = "auto";
        entry.append(title);
        const metadata = [ext.version && "Version " + ext.version, ext.author && "By " + ext.author].filter(Boolean);
        if (metadata.length) entry.append(element("p", metadata.join(" · "), "description"));
        if (ext.summary) entry.append(element("p", ext.summary));
        entry.append(element("p", compatibility(ext), "description"));
        if (ext.compatibility_reasons?.length) entry.append(textList(ext.compatibility_reasons));
        const actions = element("div", undefined, "controls");
        const present = extensions.some(item => item.extension_id === ext.addon_id);
        const stage = button(
            present ? "Installed" : reviewActionLabel,
            event =>
                sendAction(
                    "stageCatalogInstall",
                    { extension_id: ext.addon_id },
                    "Downloading and verifying the extension…",
                    event.currentTarget
                ),
            "primary-button"
        );
        if (operation?.name === "stageCatalogInstall" && operation.extensionId === ext.addon_id) {
            operation.trigger = stage;
            setCatalogReviewBusy(stage, true);
        }
        stage.disabled = present || ext.compatibility_status === "Incompatible" || byId("pick-file").disabled;
        actions.append(stage);
        if (ext.listing_url && URL.canParse(ext.listing_url)) {
            const url = new URL(ext.listing_url);
            if (url.origin === "https://addons.mozilla.org" && !url.username && !url.password) {
                const link = element("a", "View on Mozilla Add-ons");
                link.href = url.href;
                actions.append(link);
            }
        }
        entry.append(actions);
        results.append(entry);
        if (ext.addon_id === focusedEntry) (stage.disabled ? title : stage).focus();
    }
}

function search(page) {
    if (!catalogAvailable || !searchQuery) return;
    ++searchId;
    searchPage = page;
    byId("pages").hidden = true;
    catalogEntries = [];
    renderResults();
    announce(catalogStatus, "Searching Mozilla Add-ons…");
    ladybird.sendMessage("searchCatalog", { query: searchQuery, page, request_id: searchId });
}

byId("catalog-search").addEventListener("submit", event => {
    event.preventDefault();
    const submitted = query.value.trim();
    if (!submitted) {
        query.focus();
        return;
    }
    searchQuery = submitted;
    search(1);
});
byId("previous").addEventListener("click", () => search(searchPage - 1));
byId("next").addEventListener("click", () => search(searchPage + 1));

function searched(data) {
    if (!catalogAvailable || data.request_id !== searchId) return;
    if (data.error) {
        announce(catalogStatus, data.error, true);
        return;
    }
    catalogEntries = data.results || [];
    renderResults();
    announce(
        catalogStatus,
        catalogEntries.length
            ? "Results for “" + searchQuery + "” · Page " + searchPage
            : "No extensions found for “" + searchQuery + "”."
    );
    byId("pages").hidden = searchPage === 1 && !data.has_more;
    byId("page-number").textContent = "Page " + searchPage;
    byId("previous").disabled = searchPage <= 1;
    byId("next").disabled = !data.has_more;
}

byId("pick-file").addEventListener("click", event => {
    sendAction("pickLocalXpi", {}, "Choose a signed XPI file to review.", event.currentTarget);
});

function staged(data, update) {
    if (operation.name === "stageCatalogInstall") setCatalogReviewBusy(operation.trigger, false);
    if (data.error) {
        const reasons = Array.isArray(data.compatibility_reasons) ? data.compatibility_reasons : [];
        finishAction([data.error, ...reasons].join("\n"), true);
        return;
    }
    if (data.cancelled || data.up_to_date) {
        finishAction(data.up_to_date ? "The installed version is up to date." : "");
        return;
    }
    const permitted = ["Tested in Ladyflock", "Manifest compatible · Untested"].includes(data.compatibility_status);
    const complete =
        data.token &&
        data.verified_signature === true &&
        permitted &&
        [
            data.permissions,
            data.host_permissions,
            data.optional_permissions,
            data.optional_host_permissions,
            data.data_collection?.required,
            data.data_collection?.optional,
        ].every(Array.isArray) &&
        (!update || Array.isArray(data.newly_requested_permissions));
    if (!complete) {
        const message = "The package could not be verified for installation.";
        if (data.token) {
            operation.name = "cancelInstall";
            operation.error = message;
            announce(notice, message, true);
            ladybird.sendMessage("cancelInstall", { token: data.token });
        } else {
            finishAction(message, true);
        }
        return;
    }
    announce(notice, "");
    const content = element("div");
    const packageName = element("p");
    packageName.append(extensionIdentity(data, data.name + " · Version " + data.version));
    content.append(packageName);
    content.append(element("p", "Verified signature · " + data.compatibility_status, "description"));
    if (update) group(content, "New permissions in this update", data.newly_requested_permissions);
    group(content, "Required permissions", data.permissions);
    group(content, "Requested sites", data.host_permissions);
    group(content, "Declared data collection", data.data_collection.required);
    if (!data.permissions.length && !data.host_permissions.length && !data.data_collection.required.length) {
        content.append(
            element("p", "This extension declares no required permissions, site access, or data collection.")
        );
    }
    group(content, "Optional permissions", data.optional_permissions);
    group(content, "Optional sites", data.optional_host_permissions);
    group(content, "Optional data collection", data.data_collection.optional);
    const token = data.token;
    operation.name = "review";
    confirmAction(
        update ? "Review update" : "Review extension",
        content,
        update ? "Update" : "Install",
        () => {
            announce(notice, update ? "Updating extension…" : "Installing extension…");
            operation.name = update ? "approveUpdate" : "approveInstall";
            ladybird.sendMessage(operation.name, { token, consented: true });
        },
        () => {
            operation.name = "cancelInstall";
            ladybird.sendMessage("cancelInstall", { token });
        },
        operation.trigger
    );
}

const completionMessages = {
    extensionStateUpdated: ["setExtensionEnabled", "Extension settings saved."],
    extensionSiteAccessUpdated: ["setExtensionSiteAccess", "Site access saved."],
    extensionRemoved: ["removeExtension", "Extension removed."],
    extensionOptionsOpened: ["openExtensionOptions", "Extension options opened."],
    installCompleted: ["approveInstall", "Extension installed."],
    updateCompleted: ["approveUpdate", "Extension updated."],
    installCancelled: ["cancelInstall", ""],
};

document.addEventListener("WebUIMessage", event => {
    const { name, data } = event.detail;
    if (name === "extensionIcon") {
        receivedIcon(data);
    } else if (name === "installedExtensions") {
        if (!Array.isArray(data.extensions)) {
            installed.replaceChildren(
                element("p", data.error || "The extension manager is unavailable.", "description")
            );
            announce(catalogStatus, "Mozilla Add-ons search is unavailable in this session.");
            return;
        }
        renderInstalled(data);
    } else if (name === "catalogSearchResults") {
        searched(data);
    } else if (name === "installStaged" && ["pickLocalXpi", "stageCatalogInstall"].includes(operation?.name)) {
        staged(data, false);
    } else if (
        name === "updateStaged" &&
        (operation?.name === "checkManualUpdate" || (operation?.name === "pickLocalXpi" && operation.extensionId))
    ) {
        staged(data, true);
    } else if (Object.hasOwn(completionMessages, name) && operation?.name === completionMessages[name][0]) {
        if (data.summary) renderInstalled(data.summary);
        const error = data.error || operation.error;
        finishAction(error || completionMessages[name][1], Boolean(error));
    }
});

function loadInstalled() {
    ladybird.sendMessage("getInstalledExtensions", {});
}

if (window.ladybird) loadInstalled();
else document.addEventListener("WebUILoaded", loadInstalled, { once: true });
