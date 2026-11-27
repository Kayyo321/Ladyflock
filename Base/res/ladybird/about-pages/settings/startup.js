const restorePreviousSession = document.querySelector("#restore-previous-session");
const newTabPageMode = document.querySelector("#new-tab-page-mode");
const newTabPageModeDashboardOption = document.querySelector("#new-tab-page-mode-dashboard");
const newTabPagePreviewDashboard = document.querySelector("#new-tab-page-preview-dashboard");
const newTabPageURL = document.querySelector("#new-tab-page-url");
const newTabPageURLReset = document.querySelector("#new-tab-page-url-reset");
const defaultBrowserGroup = document.querySelector("#default-browser-group");
const defaultBrowserLabel = document.querySelector("#default-browser-label");
const defaultBrowserStatus = document.querySelector("#default-browser-status");
const makeDefaultBrowser = document.querySelector("#make-default-browser");

let defaultNewTabURL = "about:newtab";
let dashboardNewTabURL = "about:dashboard";
let supportsNewTabDashboard = false;
let lastCustomNewTabURL = "";

const syncNewTabModeControls = url => {
    if (!newTabPageMode)
        return;

    // Where the dashboard can't be shown, new tabs open the blank page instead.
    const isBlank = !url || url === defaultNewTabURL || (url === dashboardNewTabURL && !supportsNewTabDashboard);
    const isDashboard = url === dashboardNewTabURL;

    if (supportsNewTabDashboard && isDashboard) {
        newTabPageMode.value = "dashboard";
    } else if (isBlank) {
        newTabPageMode.value = "blank";
    } else {
        newTabPageMode.value = "custom";
        if (url && !isDashboard)
            lastCustomNewTabURL = url;
    }

    if (newTabPagePreviewDashboard)
        newTabPagePreviewDashboard.classList.toggle("hidden", !supportsNewTabDashboard);
};

const DEFAULT_BROWSER_STATUS = {
    default: "Links from other apps open in Ladybird.",
    "not-default": "Links from other apps open in another browser.",
    unavailable: "Ladybird cannot determine or set the default browser on this system.",
};

let defaultBrowser;
// Why the last attempt to make Ladybird the default browser failed, which is shown in place of the status.
let defaultBrowserFailure = null;

const updateDefaultBrowser = () => {
    const canMakeDefault = defaultBrowser === "not-default";

    defaultBrowserGroup.classList.toggle("hidden", !defaultBrowser);
    defaultBrowserStatus.textContent = defaultBrowserFailure ?? DEFAULT_BROWSER_STATUS[defaultBrowser] ?? "";
    makeDefaultBrowser.classList.toggle("hidden", !canMakeDefault);

    // A control row is filled under the pointer, and a click anywhere on it reaches the control its label names. This
    // row is one only while there is something for the button to do.
    defaultBrowserGroup.classList.toggle("control-row", canMakeDefault);
    if (canMakeDefault) {
        defaultBrowserLabel.htmlFor = makeDefaultBrowser.id;
    } else {
        defaultBrowserLabel.removeAttribute("for");
    }
};

const loadSettings = settings => {
    if (Object.hasOwn(settings, "defaultBrowser")) {
        defaultBrowser = settings.defaultBrowser;
        defaultBrowserFailure = null;
        updateDefaultBrowser();
    }

    if (Object.hasOwn(settings, "restorePreviousSession")) {
        restorePreviousSession.checked = settings.restorePreviousSession === true;
    }

    if (Object.hasOwn(settings, "defaultNewTabPageURL")) {
        defaultNewTabURL = settings.defaultNewTabPageURL;
        newTabPageURLReset.classList.remove("hidden");
        newTabPageURL.placeholder = settings.defaultNewTabPageURL;
    }
    if (Object.hasOwn(settings, "dashboardNewTabPageURL")) {
        dashboardNewTabURL = settings.dashboardNewTabPageURL;
    }
    if (Object.hasOwn(settings, "newTabPageURL")) {
        newTabPageURL.classList.remove("error");
        newTabPageURL.value = settings.newTabPageURL;
        syncNewTabModeControls(settings.newTabPageURL);
    }
};

const loadStartupFeatures = features => {
    supportsNewTabDashboard = features?.newTabDashboard === true;
    if (newTabPageModeDashboardOption) {
        newTabPageModeDashboardOption.classList.toggle("hidden", !supportsNewTabDashboard);
        newTabPageModeDashboardOption.hidden = !supportsNewTabDashboard;
        newTabPageModeDashboardOption.disabled = !supportsNewTabDashboard;
    }
    syncNewTabModeControls(newTabPageURL.value);
};

if (newTabPageMode) {
    newTabPageMode.addEventListener("change", () => {
        const mode = newTabPageMode.value;
        if (mode === "blank") {
            newTabPageURL.value = defaultNewTabURL;
            newTabPageURL.dispatchEvent(new Event("change"));
        } else if (mode === "dashboard" && supportsNewTabDashboard) {
            newTabPageURL.value = dashboardNewTabURL;
            newTabPageURL.dispatchEvent(new Event("change"));
        } else if (mode === "custom") {
            if (lastCustomNewTabURL) {
                newTabPageURL.value = lastCustomNewTabURL;
                newTabPageURL.dispatchEvent(new Event("change"));
            }
            newTabPageURL.focus();
            newTabPageURL.select();
        }
    });
}

if (newTabPagePreviewDashboard) {
    newTabPagePreviewDashboard.addEventListener("click", () => {
        if (!supportsNewTabDashboard)
            return;
        window.open(dashboardNewTabURL, "_blank", "noopener");
    });
}

// The system may ask before it changes the default browser, so the button waits for its answer.
makeDefaultBrowser.addEventListener("click", () => {
    makeDefaultBrowser.disabled = true;
    defaultBrowserFailure = null;
    updateDefaultBrowser();

    ladybird.sendMessage("makeDefaultBrowser");
});

restorePreviousSession.addEventListener("change", () => {
    ladybird.sendMessage("setRestorePreviousSession", restorePreviousSession.checked);
});

newTabPageURL.addEventListener("change", () => {
    newTabPageURL.classList.remove("success");
    newTabPageURL.classList.remove("error");

    newTabPageURL.value = ensureURLHasScheme(newTabPageURL.value);

    if (!containsValidURL(newTabPageURL)) {
        newTabPageURL.classList.add("error");
        return;
    }

    ladybird.sendMessage("setNewTabPageURL", newTabPageURL.value);
    syncNewTabModeControls(newTabPageURL.value);
    newTabPageURL.classList.add("success");

    setTimeout(() => {
        newTabPageURL.classList.remove("success");
    }, 1000);
});

newTabPageURLReset.addEventListener("click", () => {
    newTabPageURL.value = newTabPageURL.placeholder;
    newTabPageURL.dispatchEvent(new Event("change"));
});

document.addEventListener("WebUIMessage", event => {
    if (event.detail.name === "loadFeatures") {
        loadStartupFeatures(event.detail.data);
    } else if (event.detail.name === "loadSettings") {
        loadSettings(event.detail.data);
    } else if (event.detail.name === "defaultBrowserResult") {
        makeDefaultBrowser.disabled = false;
        defaultBrowserFailure = event.detail.data;
        updateDefaultBrowser();
    }
});
