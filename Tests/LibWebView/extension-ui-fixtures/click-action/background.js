let count = 0;
browser.action.setBadgeBackgroundColor({ color: "#ffe900" });
browser.action.onClicked.addListener(() => {
    browser.action.setBadgeText({ text: String(++count) });
});
