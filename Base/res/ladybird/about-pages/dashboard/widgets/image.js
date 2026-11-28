/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 */

import { icon } from "../icons.js";
import {
    ASCII_DENSITY_MAX,
    ASCII_DENSITY_MIN,
    ASCII_DENSITY_STEP,
    compileImageShader,
    createImageShaderRenderer,
    normalizeAsciiDensity,
} from "./image-shader-renderer.js";

function button(text, className = "dash-btn") {
    const element = document.createElement("button");
    element.type = "button";
    element.className = className;
    element.textContent = text;
    return element;
}

function readImageFile(file) {
    return new Promise((resolve, reject) => {
        const reader = new FileReader();
        reader.onload = () => resolve(reader.result);
        reader.onerror = () => reject(new Error("This file couldn’t be read."));
        reader.readAsDataURL(file);
    });
}

function modeControl(title, className, choices, onSelect) {
    const row = document.createElement("div");
    row.className = "dash-row-between image-mode-row";
    const label = document.createElement("span");
    label.textContent = title;
    const buttons = document.createElement("div");
    buttons.className = `dash-segments ${className}`;
    buttons.setAttribute("role", "group");
    buttons.setAttribute("aria-label", title);
    for (const [mode, text, description] of choices) {
        const choice = button(text);
        choice.dataset.mode = mode;
        choice.title = description;
        choice.addEventListener("click", () => onSelect(mode));
        buttons.append(choice);
    }
    row.append(label, buttons);
    return {
        row,
        update(mode, busy) {
            for (const choice of buttons.children) {
                choice.disabled = busy;
                choice.setAttribute("aria-pressed", String(mode === choice.dataset.mode));
            }
        },
    };
}

function shaderHelp() {
    const details = document.createElement("details");
    details.className = "image-shader-help";
    const summary = document.createElement("summary");
    summary.textContent = "Shader file format";
    const description = document.createElement("p");
    description.textContent = "Import a .frag or .glsl file to use it on any image. Write one shade function with optional initialized variables and a final return.";
    const inputs = document.createElement("p");
    inputs.textContent = "The color parameter is the source pixel. Use sampleImage(uv) to sample normalized image coordinates (0–1) and time for elapsed seconds.";
    const example = document.createElement("pre");
    example.className = "mono";
    example.textContent = "vec4 shade(vec2 uv, vec4 color, float time) {\n    return vec4(1.0 - color.rgb, color.a);\n}";
    details.append(summary, description, example, inputs);
    return details;
}

function builtInShader(id) {
    return id === "default" || id === "breeze";
}

export function mount(root, api) {
    const { instanceId } = api;
    const image = document.createElement("img");
    image.className = "dashboard-image";
    image.alt = "Dashboard image";
    image.draggable = false;
    const canvas = document.createElement("canvas");
    canvas.className = "dashboard-image-shader";
    canvas.hidden = true;
    canvas.setAttribute("role", "img");
    canvas.setAttribute("aria-label", image.alt);

    const add = button("Add image", "dash-btn image-add");
    add.prepend(icon("plus", 14));
    add.addEventListener("click", () => api.openSettings());

    let mounted = true;
    let loadGeneration = 0;
    let shaderGeneration = 0;
    let busy = false;
    let refreshSettings = () => {};
    let refreshLibraryIfOpen = () => {};
    let disposeSettings = () => {};
    let config = api.config;
    let program = null;
    let shaderError = "";
    let previewDensity = null;
    const renderer = createImageShaderRenderer(image, canvas, reportShaderError);

    function reportShaderError(error) {
        const message = error.message || "Couldn’t display the selected shader.";
        if (message !== shaderError)
            api.announce(message);
        shaderError = message;
        refreshSettings();
    }

    function updateOutput() {
        if (!mounted)
            return;
        shaderError = "";
        const shaderId = config.shaderId || "default";
        const resizeMode = config.resizeMode === "crop" ? "crop" : "resize";
        image.dataset.mode = resizeMode;
        renderer.update({
            shaderId: builtInShader(shaderId) || program ? shaderId : "default",
            program,
            renderMode: config.shaderMode === "ascii" ? "ascii" : "blit",
            resizeMode,
            asciiDensity: previewDensity ?? normalizeAsciiDensity(config.asciiDensity),
        });
    }

    async function loadShader() {
        const generation = ++shaderGeneration;
        const id = config.shaderId || "default";
        program = null;
        updateOutput();
        if (builtInShader(id))
            return;
        try {
            const { source } = await api.request("shaders.load", { id });
            if (mounted && generation === shaderGeneration && source) {
                program = compileImageShader(source);
                updateOutput();
            }
        } catch (err) {
            if (mounted && generation === shaderGeneration)
                reportShaderError(err);
        }
    }

    function showImage(dataUrl) {
        if (dataUrl && dataUrl !== image.getAttribute("src"))
            image.src = dataUrl;
        else if (!dataUrl)
            image.removeAttribute("src");
        if (dataUrl && root.firstElementChild !== image)
            root.replaceChildren(image, canvas);
        else if (!dataUrl && root.firstElementChild !== add)
            root.replaceChildren(add);
        updateOutput();
        refreshSettings();
    }

    image.addEventListener("error", () => {
        if (!mounted || !image.hasAttribute("src"))
            return;
        root.replaceChildren(add);
        api.announce("This image couldn’t be displayed. Choose another image in settings.");
    });

    async function load() {
        const generation = ++loadGeneration;
        try {
            const { dataUrl } = await api.request("images.load", { instanceId });
            if (mounted && generation === loadGeneration)
                showImage(dataUrl);
        } catch (err) {
            if (mounted && generation === loadGeneration) {
                if (!root.childElementCount)
                    root.append(add);
                api.announce(err.message || "Couldn’t load the image.");
            }
        }
    }

    const unsubscribeImage = api.onEvent("imageChanged", event => {
        if (event.instanceId === instanceId)
            load();
    });
    const unsubscribeShaders = api.onEvent("shadersChanged", event => {
        if (event.id === config.shaderId)
            loadShader();
        refreshLibraryIfOpen();
    });
    loadShader();
    load();

    return {
        onUpdate(next) {
            const previous = config;
            config = next.config;
            if (previous.shaderId !== config.shaderId)
                loadShader();
            else
                updateOutput();
            refreshSettings();
        },
        renderConfig(container, close) {
            disposeSettings();
            const fileLabel = document.createElement("label");
            fileLabel.className = "dash-col";
            fileLabel.textContent = "Image file";
            const file = document.createElement("input");
            file.id = "image-file";
            file.type = "file";
            file.accept = "image/png,image/jpeg,image/gif,image/webp,image/avif,image/bmp";
            fileLabel.append(file);
            container.append(fileLabel);

            const url = document.createElement("input");
            const importUrl = button("Import", "dash-btn image-import-url");
            if (api.context.allowsNetworkWidgets) {
                const form = document.createElement("form");
                form.className = "dash-col";
                const label = document.createElement("label");
                label.htmlFor = "image-url";
                label.textContent = "Image URL";
                url.id = label.htmlFor;
                url.type = "url";
                url.required = true;
                url.placeholder = "https://";
                url.className = "dash-input dash-grow";
                importUrl.type = "submit";
                const row = document.createElement("div");
                row.className = "dash-row";
                row.append(url, importUrl);
                form.append(label, row);
                form.addEventListener("submit", event => {
                    event.preventDefault();
                    importImage(async () => {
                        const parsed = new URL(url.value.trim());
                        if (parsed.protocol !== "https:")
                            throw new Error("Enter an HTTPS image URL.");
                        const result = await api.request("images.fetchUrl", { instanceId, url: parsed.href });
                        return result.dataUrl;
                    });
                });
                container.append(form);
            }

            const modes = modeControl("When resizing", "image-resize-mode", [
                ["resize", "Resize", "Fit the whole image inside the widget"],
                ["crop", "Crop", "Fill the widget and crop the edges"],
            ], mode => run(() => saveConfig({ resizeMode: mode })));
            container.append(modes.row);

            const effects = document.createElement("div");
            effects.className = "image-effects";
            const shaderField = document.createElement("div");
            shaderField.className = "dash-col";
            const shaderLabel = document.createElement("label");
            shaderLabel.textContent = "Shader";
            shaderLabel.htmlFor = "image-shader";
            const shaderRow = document.createElement("div");
            shaderRow.className = "dash-row";
            const shader = document.createElement("select");
            shader.className = "dash-select dash-grow";
            shader.id = shaderLabel.htmlFor;
            shader.append(new Option("Default", "default"), new Option("Breeze", "breeze"));
            const saved = document.createElement("optgroup");
            saved.label = "Saved shaders";
            const shaderFile = document.createElement("input");
            shaderFile.type = "file";
            shaderFile.id = "image-shader-file";
            shaderFile.accept = ".frag,.glsl";
            shaderFile.hidden = true;
            const importShader = button("Import shader…", "dash-btn image-import-shader");
            importShader.addEventListener("click", () => shaderFile.click());
            shaderRow.append(shader, importShader, shaderFile);
            shaderField.append(shaderLabel, shaderRow);

            const removeShader = button("Remove from library…", "dash-btn ghost image-remove-shader");
            const confirmRow = document.createElement("div");
            confirmRow.className = "image-shader-confirm hidden";
            const confirmText = document.createElement("p");
            confirmText.textContent = "Remove this shader from every Image widget?";
            const cancel = button("Cancel");
            const confirm = button("Remove", "dash-btn image-confirm-remove");
            confirmRow.append(confirmText, cancel, confirm);
            let confirmingId = null;
            const outputModes = modeControl("Output", "image-shader-mode", [
                ["blit", "Blit RGB", "Display the shaded image in full color"],
                ["ascii", "ASCII art", "Use darker or lighter characters to match the shaded image"],
            ], mode => runShader(() => saveConfig({ shaderMode: mode })));
            const densityField = document.createElement("div");
            densityField.className = "dash-col image-ascii-density";
            const densityHeading = document.createElement("div");
            densityHeading.className = "dash-row-between image-mode-row";
            const densityLabel = document.createElement("label");
            densityLabel.htmlFor = "image-ascii-density";
            densityLabel.textContent = "Character density";
            const density = document.createElement("input");
            density.id = densityLabel.htmlFor;
            density.type = "range";
            density.min = ASCII_DENSITY_MIN;
            density.max = ASCII_DENSITY_MAX;
            density.step = ASCII_DENSITY_STEP;
            const densityValue = document.createElement("output");
            densityValue.htmlFor.add(density.id);
            densityHeading.append(densityLabel, densityValue);
            densityField.append(densityHeading, density);
            function showDensity(value) {
                density.value = value;
                densityValue.value = `${value} chars/in²`;
                density.setAttribute("aria-valuetext", `${value} characters per square inch`);
            }
            // A held arrow key can choose another value while the previous change is saving.
            // Keep the slider focused and commit the latest choice after the bridge replies.
            let savingDensity = false;
            let densityToSave = null;
            function saveDensity() {
                if (!isOpen() || (busy && !savingDensity))
                    return;
                densityToSave = density.valueAsNumber;
                if (savingDensity)
                    return;
                if (densityToSave === normalizeAsciiDensity(config.asciiDensity)) {
                    previewDensity = densityToSave = null;
                    return;
                }
                savingDensity = true;
                runShader(async () => {
                    try {
                        while (mounted && densityToSave !== null) {
                            const value = densityToSave;
                            densityToSave = null;
                            await saveConfig({ asciiDensity: value });
                            if (previewDensity === value)
                                previewDensity = null;
                        }
                    } catch (err) {
                        previewDensity = densityToSave = null;
                        throw err;
                    } finally {
                        savingDensity = false;
                        updateOutput();
                    }
                });
            }
            let draggingDensity = false;
            function endDensityDrag(event) {
                draggingDensity = false;
                window.removeEventListener("pointerup", endDensityDrag);
                window.removeEventListener("pointercancel", endDensityDrag);
                if (event?.type === "pointerup")
                    saveDensity();
                else if (previewDensity !== null) {
                    previewDensity = null;
                    updateOutput();
                    refreshSettings();
                }
            }
            density.addEventListener("pointerdown", () => {
                draggingDensity = true;
                window.addEventListener("pointerup", endDensityDrag);
                window.addEventListener("pointercancel", endDensityDrag);
            });
            density.addEventListener("input", () => {
                previewDensity = density.valueAsNumber;
                showDensity(previewDensity);
                updateOutput();
                if (!draggingDensity)
                    saveDensity();
            });
            density.addEventListener("change", saveDensity);
            const outputError = document.createElement("p");
            outputError.className = "image-shader-error form-error hidden";
            const shaderStatus = document.createElement("p");
            shaderStatus.className = "image-shader-status hidden";
            shaderStatus.setAttribute("role", "status");
            effects.append(shaderField, removeShader, confirmRow, outputModes.row, densityField, outputError, shaderStatus, shaderHelp());
            const advanced = document.createElement("details");
            advanced.className = "image-advanced-settings";
            const advancedSummary = document.createElement("summary");
            advancedSummary.textContent = "Advanced Settings…";
            advanced.append(advancedSummary, effects);
            container.append(advanced);

            const status = document.createElement("p");
            status.className = "image-status hidden";
            status.setAttribute("role", "status");
            const remove = button("Remove image", "dash-btn image-remove");
            const footer = document.createElement("div");
            footer.className = "dialog-footer";
            footer.append(remove);
            container.append(status, footer);

            const dialog = container.closest("dialog");
            const isOpen = () => mounted && file.isConnected && dialog.open;
            function updateControls() {
                file.disabled = url.disabled = importUrl.disabled = remove.disabled = busy;
                shader.disabled = importShader.disabled = removeShader.disabled = cancel.disabled = confirm.disabled = busy;
                const id = config.shaderId || "default";
                shader.value = id;
                if (!shader.value)
                    shader.value = "default";
                shader.title = shader.selectedOptions[0]?.textContent || "";
                if (confirmingId !== shader.value)
                    confirmingId = null;
                removeShader.classList.toggle("hidden", builtInShader(shader.value) || !!confirmingId);
                confirmRow.classList.toggle("hidden", !confirmingId);
                outputError.textContent = shaderError;
                outputError.classList.toggle("hidden", !shaderError);
                footer.classList.toggle("hidden", !image.hasAttribute("src"));
                modes.update(image.dataset.mode, busy);
                outputModes.update(config.shaderMode === "ascii" ? "ascii" : "blit", busy);
                densityField.classList.toggle("hidden", config.shaderMode !== "ascii");
                density.disabled = busy && !savingDensity;
                showDensity(previewDensity ?? normalizeAsciiDensity(config.asciiDensity));
            }
            refreshSettings = updateControls;
            refreshSettings();

            let listGeneration = 0;
            async function refreshLibrary() {
                const generation = ++listGeneration;
                try {
                    const { shaders } = await api.request("shaders.list");
                    if (!isOpen() || generation !== listGeneration)
                        return;
                    saved.replaceChildren(...shaders.map(entry => new Option(entry.name, entry.id)));
                    if (shaders.length)
                        shader.append(saved);
                    else
                        saved.remove();
                    refreshSettings();
                } catch (err) {
                    if (isOpen() && generation === listGeneration)
                        showError(err, shaderStatus);
                }
            }
            refreshLibraryIfOpen = () => {
                if (isOpen())
                    refreshLibrary();
            };
            function dialogClosed() {
                if (!file.isConnected || !dialog.open)
                    disposeSettings();
            }
            disposeSettings = () => {
                dialog.removeEventListener("close", dialogClosed);
                endDensityDrag();
                refreshSettings = () => {};
                refreshLibraryIfOpen = () => {};
                disposeSettings = () => {};
            };
            dialog.addEventListener("close", dialogClosed);
            refreshLibrary();

            function showError(err, target) {
                target.textContent = err.message || "Couldn’t update this image.";
                target.classList.add("form-error");
                target.classList.remove("hidden");
            }

            async function saveConfig(patch) {
                if (!await api.updateConfig(patch))
                    throw new Error("Couldn’t save these image settings. Please try again.");
            }

            shader.addEventListener("change", () => {
                const id = shader.value;
                runShader(() => saveConfig({ shaderId: id }));
            });
            shaderFile.addEventListener("change", () => {
                const selected = shaderFile.files[0];
                shaderFile.value = "";
                if (!selected)
                    return;
                runShader(async () => {
                    if (selected.size > api.context.maxShaderBytes)
                        throw new Error(`Choose a shader of ${api.context.maxShaderBytes / 1024} KiB or less.`);
                    const source = await selected.text();
                    compileImageShader(source);
                    if (!isOpen())
                        return;
                    const name = selected.name.replace(/\.(frag|glsl)$/i, "") || selected.name;
                    const entry = await api.request("shaders.save", { name, source });
                    if (!mounted)
                        return;
                    await saveConfig({ shaderId: entry.id });
                    if (isOpen())
                        await refreshLibrary();
                    api.announce(`${entry.name} is saved for all Image widgets.`);
                });
            });
            removeShader.addEventListener("click", () => {
                confirmingId = shader.value;
                refreshSettings();
                cancel.focus();
            });
            cancel.addEventListener("click", () => {
                confirmingId = null;
                refreshSettings();
                removeShader.focus();
            });
            confirm.addEventListener("click", () => {
                const id = confirmingId;
                runShader(async () => {
                    await api.request("shaders.remove", { id });
                    confirmingId = null;
                    await refreshLibrary();
                }).then(() => {
                    if (isOpen() && builtInShader(shader.value))
                        shader.focus();
                });
            });

            function runShader(action) {
                return run(action, "", shaderStatus);
            }

            async function run(action, message = "", target = status) {
                if (busy || !isOpen())
                    return;
                const focused = document.activeElement;
                busy = true;
                refreshSettings();
                target.textContent = message;
                target.classList.remove("form-error");
                target.classList.toggle("hidden", !message);
                try {
                    await action();
                } catch (err) {
                    if (isOpen())
                        showError(err, target);
                } finally {
                    busy = false;
                    refreshSettings();
                    if (isOpen() && focused && container.contains(focused) && document.activeElement === document.body)
                        focused.focus();
                }
            }

            function finish(dataUrl) {
                if (!mounted)
                    return;
                ++loadGeneration;
                showImage(dataUrl);
                if (isOpen())
                    close();
            }

            function importImage(read) {
                return run(async () => {
                    const dataUrl = await read();
                    if (!isOpen())
                        return;
                    const marker = ";base64,";
                    const index = dataUrl.indexOf(marker);
                    if (!dataUrl.startsWith("data:") || index === -1)
                        throw new Error("Choose a supported image file.");
                    const preview = new Image();
                    preview.src = dataUrl;
                    try {
                        await preview.decode();
                    } catch {
                        throw new Error("This image couldn’t be decoded. Choose another image.");
                    }
                    if (!preview.naturalWidth || !preview.naturalHeight)
                        throw new Error("This image has no displayable size.");
                    if (isOpen()) {
                        const result = await api.request("images.save", { instanceId, base64: dataUrl.slice(index + marker.length) });
                        finish(result.dataUrl);
                    }
                }, "Importing image…");
            }

            file.addEventListener("change", () => {
                const selected = file.files[0];
                file.value = "";
                if (selected) {
                    importImage(() => {
                        if (selected.size > api.context.maxImageBytes)
                            throw new Error(`Choose an image of ${api.context.maxImageBytes / (1024 * 1024)} MiB or less.`);
                        return readImageFile(selected);
                    });
                }
            });
            remove.addEventListener("click", () => run(async () => {
                await api.request("images.remove", { instanceId });
                finish(null);
            }));
        },
        unmount() {
            mounted = false;
            disposeSettings();
            unsubscribeImage();
            unsubscribeShaders();
            renderer.dispose();
        },
    };
}
