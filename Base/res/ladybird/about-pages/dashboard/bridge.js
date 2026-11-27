/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 */

let nextRequestId = 1;
const pendingRequests = new Map();
const eventListeners = new Map();
let expectedGeneration = null;
let bridgeReady = false;
let readyResolvers = [];

function resolveReady() {
    if (bridgeReady)
        return;
    bridgeReady = true;
    const resolvers = readyResolvers;
    readyResolvers = [];
    for (const fn of resolvers)
        fn();
}

// Install listeners before checking whether window.ladybird is already connected.
document.addEventListener("WebUILoaded", () => {
    resolveReady();
});

document.addEventListener("WebUIMessage", event => {
    const detail = event.detail;
    if (!detail)
        return;
    if (detail.name === "dashboardReply" && detail.data) {
        const data = detail.data;
        if (expectedGeneration !== null && data.documentGeneration && data.documentGeneration !== expectedGeneration)
            return;
        const entry = pendingRequests.get(data.requestId);
        if (!entry)
            return;
        pendingRequests.delete(data.requestId);
        clearTimeout(entry.timer);
        if (data.ok) {
            entry.resolve(data.data ?? {});
        } else {
            const err = new Error(data.error || "Request failed");
            err.conflict = Boolean(data.conflict);
            err.stale = Boolean(data.stale);
            err.missing = Boolean(data.missing);
            err.payload = data;
            entry.reject(err);
        }
    } else if (detail.name === "dashboardEvent" && detail.data) {
        const data = detail.data;
        if (expectedGeneration !== null && data.documentGeneration && data.documentGeneration !== expectedGeneration)
            return;
        const handlers = eventListeners.get(data.event);
        if (!handlers)
            return;
        for (const fn of [...handlers]) {
            try {
                fn(data.data);
            } catch (e) {
                console.error("Dashboard event handler error:", e);
            }
        }
    }
});

if (typeof window.ladybird !== "undefined" && typeof window.ladybird.sendMessage === "function") {
    resolveReady();
}

export function setExpectedGeneration(gen) {
    expectedGeneration = gen;
}

export function waitForBridge(timeoutMs = 2500) {
    if (bridgeReady && typeof window.ladybird !== "undefined")
        return Promise.resolve(true);
    return new Promise(resolve => {
        const timer = setTimeout(() => {
            resolve(typeof window.ladybird !== "undefined" && typeof window.ladybird.sendMessage === "function");
        }, timeoutMs);
        readyResolvers.push(() => {
            clearTimeout(timer);
            resolve(true);
        });
    });
}

export function request(method, params = {}, timeoutMs = 15000) {
    if (typeof window.ladybird === "undefined" || typeof window.ladybird.sendMessage !== "function") {
        return Promise.reject(new Error("Dashboard bridge is unavailable"));
    }
    const requestId = nextRequestId++;
    return new Promise((resolve, reject) => {
        const timer = setTimeout(() => {
            pendingRequests.delete(requestId);
            reject(new Error(`Dashboard request timed out: ${method}`));
        }, timeoutMs);
        pendingRequests.set(requestId, { resolve, reject, timer });
        window.ladybird.sendMessage("dashboardRequest", { requestId, method, params });
    });
}

export function onDashboardEvent(eventName, handler) {
    if (!eventListeners.has(eventName))
        eventListeners.set(eventName, new Set());
    eventListeners.get(eventName).add(handler);
    return () => {
        const set = eventListeners.get(eventName);
        if (set)
            set.delete(handler);
    };
}
