/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 */

const defaultSource = `vec4 shade(vec2 uv, vec4 color, float time) { return color; }`;
export const ASCII_DENSITY_MIN = 80;
export const ASCII_DENSITY_MAX = 560;
export const ASCII_DENSITY_DEFAULT = 330;
export const ASCII_DENSITY_STEP = 10;

export function normalizeAsciiDensity(value) {
    if (typeof value !== "number" || !Number.isFinite(value))
        return ASCII_DENSITY_DEFAULT;
    return Math.max(ASCII_DENSITY_MIN, Math.min(ASCII_DENSITY_MAX, value));
}

const breezeSource = `vec4 shade(vec2 uv, vec4 color, float time) {
    float wave = sin(uv.y * 13.0 + time * 1.3 + 2.0 * sin(uv.x * 4.0));
    vec2 drift = vec2(wave * 0.012, 0.004 * sin(uv.x * 9.0 + time));
    vec4 leaf = sampleImage(uv + drift);
    float light = smoothstep(-0.6, 1.0, wave);
    vec3 cool = leaf.rgb * vec3(0.83, 1.03, 1.08) + vec3(0.015, 0.04, 0.045);
    vec3 sun = vec3(1.0, 0.94, 0.72);
    return vec4(mix(cool, sun, light * 0.18), leaf.a);
}`;

const types = new Set(["float", "vec2", "vec3", "vec4"]);
const functions = new Set(`abs acos asin atan ceil clamp cos degrees distance dot exp exp2 floor fract
    length log log2 max min mix mod normalize pow radians sign sin smoothstep sqrt step tan sampleImage`.split(/\s+/));

// Admit a straight-line GLSL function, never an entire translation unit. Only declared values,
// arithmetic, vector swizzles and these named functions can enter a generated fragment shader.
function validateSource(source) {
    if (typeof source !== "string" || source.length > 16384)
        throw new Error("Choose a shader of 16 KiB or less.");
    const clean = source.replace(/\/\*[\s\S]*?\*\/|\/\/[^\r\n]*/g, " ").trim();
    const header = /^vec4\s+shade\s*\(\s*vec2\s+uv\s*,\s*vec4\s+color\s*,\s*float\s+time\s*\)\s*\{([^{}]*)\}$/;
    const body = clean.match(header)?.[1];
    if (!body)
        throw new Error("Use a single vec4 shade(vec2 uv, vec4 color, float time) function.");
    const statements = body.split(";").map(s => s.trim());
    if (statements.pop() !== "" || !statements.length || statements.length > 16)
        throw new Error("Use up to 16 statements, each ending in a semicolon.");
    const names = new Set(["uv", "color", "time"]);
    let animated = false;
    let tokens = 0;
    let samples = 0;
    for (let i = 0; i < statements.length; ++i) {
        const last = i === statements.length - 1;
        const statement = statements[i];
        const assignment = last ? null : statement.match(/^(float|vec[234])\s+([A-Za-z][A-Za-z0-9_]*)\s*=\s*(.+)$/s);
        const expression = last ? statement.match(/^return\s+(.+)$/s)?.[1] : assignment?.[3];
        if (!expression)
            throw new Error("Use initialized float or vector declarations followed by one return statement.");
        const identifier = assignment?.[2];
        if (identifier && (names.has(identifier) || functions.has(identifier) || types.has(identifier) || identifier.startsWith("gl_")))
            throw new Error(`Choose a different variable name for ${identifier}.`);
        const lexer = /\s+|(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?|[A-Za-z][A-Za-z0-9_]*|\.[rgbaxyzwstpq]{1,4}|[(),+*/-]/gy;
        let position = 0;
        while (position < expression.length) {
            const token = lexer.exec(expression)?.[0];
            if (!token)
                throw new Error(`Unsupported expression in statement ${i + 1}.`);
            position = lexer.lastIndex;
            if (!token.trim())
                continue;
            if (++tokens > 256)
                throw new Error("Use at most 256 expression tokens per shader.");
            if (/^[A-Za-z]/.test(token) && !names.has(token) && !functions.has(token) && !types.has(token))
                throw new Error(`Unknown shader name “${token}”.`);
            if (token === "time")
                animated = true;
            if (token === "sampleImage" && ++samples > 8)
                throw new Error("Use at most eight image samples per shader.");
        }
        if (identifier)
            names.add(identifier);
    }
    return { source: clean, animated };
}

const vertexSource = `
    attribute vec2 position;
    varying vec2 tileUV;
    void main() {
        gl_Position = vec4(position, 0.0, 1.0);
        tileUV = vec2(0.5 + position.x * 0.5, 0.5 - position.y * 0.5);
    }`;

// The glyph atlas is shared by all tiles. The cell is taller than it is wide, matching
// the monospace font. All pixels of a cell use its center to sample the shaded image.
const glyphs = ".:;=+ox%#@";
const fragmentSource = source => `
    precision highp float;
    varying vec2 tileUV;
    uniform sampler2D photo;
    uniform sampler2D atlas;
    uniform vec4 crop;
    uniform vec2 cells;
    uniform float seconds;
    uniform bool ascii;
    vec4 sampleImage(vec2 uv) { return texture2D(photo, clamp(uv, 0.0, 1.0)); }
    ${source}
    void main() {
        vec2 cell = tileUV * cells;
        vec2 position = ascii ? min((floor(cell) + 0.5) / cells, vec2(1.0)) : tileUV;
        vec2 uv = crop.xy + position * crop.zw;
        vec4 color = clamp(shade(uv, sampleImage(uv), seconds), 0.0, 1.0);
        if (ascii) {
            float tone = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
            float glyph = floor(tone * ${glyphs.length - 1}.0 + 0.5);
            float ink = texture2D(atlas, vec2((glyph + fract(cell.x)) / ${glyphs.length}.0, fract(cell.y))).a;
            float paper = 0.035;
            float lettering = 0.09 + 0.91 * smoothstep(0.08, 0.92, tone);
            color.rgb = vec3(mix(paper, lettering, ink));
        }
        // Canvas compositing consumes premultiplied pixels. Shader authors work in straight RGBA.
        gl_FragColor = vec4(color.rgb * color.a, color.a);
    }`;

function createTexture(gl, source) {
    const texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, texture);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA, gl.RGBA, gl.UNSIGNED_BYTE, source);
    if (gl.getError() !== gl.NO_ERROR) {
        gl.deleteTexture(texture);
        throw new Error("Couldn’t prepare the shader texture.");
    }
    return texture;
}

function glyphAtlas() {
    const atlas = document.createElement("canvas");
    atlas.width = glyphs.length * 16;
    atlas.height = 28;
    const context = atlas.getContext("2d");
    context.font = "bold 23px monospace";
    context.textAlign = "center";
    context.fillStyle = "white";
    for (let i = 0; i < glyphs.length; ++i)
        context.fillText(glyphs[i], i * 16 + 8, 22);
    return atlas;
}

function halfSize(source) {
    const canvas = document.createElement("canvas");
    canvas.width = source.width / 2;
    canvas.height = source.height / 2;
    canvas.getContext("2d").drawImage(source, 0, 0, canvas.width, canvas.height);
    return canvas;
}

function compileStage(gl, type, source) {
    const stage = gl.createShader(type);
    gl.shaderSource(stage, source);
    gl.compileShader(stage);
    if (!gl.getShaderParameter(stage, gl.COMPILE_STATUS)) {
        const message = gl.getShaderInfoLog(stage);
        gl.deleteShader(stage);
        throw new Error(`Shader compilation failed. ${message || "Check the types in your expression."}`);
    }
    return stage;
}

let engine;
function graphics() {
    if (engine && !engine.gl.isContextLost())
        return engine;
    const surface = document.createElement("canvas");
    const gl = surface.getContext("webgl", { antialias: false, depth: false, stencil: false, premultipliedAlpha: true });
    if (!gl)
        throw new Error("WebGL is unavailable. The original image is displayed.");
    const vertex = compileStage(gl, gl.VERTEX_SHADER, vertexSource);
    const buffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, buffer);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    gl.enableVertexAttribArray(0);
    gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
    gl.activeTexture(gl.TEXTURE1);
    // Prefilter the glyphs at 16, 8 and 4 pixels wide. A single bilinear lookup in the
    // full-size atlas misses thin strokes when a high-density cell is only three pixels wide.
    const atlas = glyphAtlas();
    const medium = halfSize(atlas);
    const atlases = [createTexture(gl, halfSize(medium)), createTexture(gl, medium), createTexture(gl, atlas)];
    gl.activeTexture(gl.TEXTURE0);
    const programs = new Map();
    function use(source) {
        let compiled = programs.get(source);
        if (!compiled) {
            const fragment = compileStage(gl, gl.FRAGMENT_SHADER, fragmentSource(source));
            const program = gl.createProgram();
            gl.attachShader(program, vertex);
            gl.attachShader(program, fragment);
            gl.bindAttribLocation(program, 0, "position");
            gl.linkProgram(program);
            gl.deleteShader(fragment);
            if (!gl.getProgramParameter(program, gl.LINK_STATUS)) {
                gl.deleteProgram(program);
                throw new Error("Couldn’t link the shader. Check its return type.");
            }
            const uniforms = {};
            for (const name of ["photo", "atlas", "crop", "cells", "seconds", "ascii"])
                uniforms[name] = gl.getUniformLocation(program, name);
            compiled = { program, uniforms };
            if (programs.size === 16) {
                const oldest = programs.keys().next().value;
                gl.deleteProgram(programs.get(oldest).program);
                programs.delete(oldest);
            }
        }
        programs.delete(source);
        programs.set(source, compiled);
        gl.useProgram(compiled.program);
        return compiled.uniforms;
    }
    engine = { gl, surface, use, atlases };
    return engine;
}

export function compileImageShader(source) {
    const shader = validateSource(source);
    graphics().use(shader.source);
    return shader;
}

function textureForImage(gl, image) {
    // Image imports may be much larger than a tile or the GPU texture limit.
    const limit = Math.min(1536, gl.getParameter(gl.MAX_TEXTURE_SIZE));
    const scale = Math.min(1, limit / Math.max(image.naturalWidth, image.naturalHeight));
    if (scale === 1)
        return createTexture(gl, image);
    const sample = document.createElement("canvas");
    sample.width = Math.max(1, Math.round(image.naturalWidth * scale));
    sample.height = Math.max(1, Math.round(image.naturalHeight * scale));
    sample.getContext("2d").drawImage(image, 0, 0, sample.width, sample.height);
    return createTexture(gl, sample);
}

const identity = { source: defaultSource, animated: false };
const breeze = { source: breezeSource, animated: true };

export function createImageShaderRenderer(image, canvas, onError) {
    const motion = matchMedia("(prefers-reduced-motion: reduce)");
    let config = { shaderId: "default", renderMode: "blit", resizeMode: "resize" };
    let frame = 0;
    let timer = 0;
    let visible = false;
    let disposed = false;
    let cachedImage;
    let lastError = "";
    canvas.hidden = true;

    function showOriginal() {
        canvas.hidden = true;
        image.style.visibility = "";
    }

    function clearTexture() {
        if (cachedImage)
            cachedImage.gl.deleteTexture(cachedImage.texture);
        cachedImage = null;
    }

    function stop() {
        clearTimeout(timer);
        cancelAnimationFrame(frame);
        frame = timer = 0;
    }

    function schedule() {
        stop();
        if (disposed)
            return;
        if (config.shaderId === "default" && config.renderMode !== "ascii") {
            clearTexture();
            showOriginal();
        } else if (visible && !document.hidden && image.complete && image.naturalWidth && image.hasAttribute("src")) {
            frame = requestAnimationFrame(draw);
        }
    }

    function draw(time) {
        frame = 0;
        const rect = image.getBoundingClientRect();
        if (rect.width < 1 || rect.height < 1)
            return;
        try {
            const shader = config.shaderId === "default" ? identity : config.shaderId === "breeze" ? breeze : config.program;
            if (!shader)
                throw new Error("The selected shader is unavailable.");
            const { gl, surface, use, atlases } = graphics();
            // Bound the amount of fragment work even on very large or high-DPI dashboards.
            const scale = Math.min(devicePixelRatio || 1, 2, 2048 / Math.max(rect.width, rect.height), Math.sqrt(786432 / (rect.width * rect.height)));
            const width = Math.max(1, Math.round(rect.width * scale));
            const height = Math.max(1, Math.round(rect.height * scale));
            const crop = config.resizeMode === "crop";
            const fit = (crop ? Math.max : Math.min)(rect.width / image.naturalWidth, rect.height / image.naturalHeight);
            const fittedWidth = image.naturalWidth * fit;
            const fittedHeight = image.naturalHeight * fit;
            const displayWidth = crop ? rect.width : fittedWidth;
            const displayHeight = crop ? rect.height : fittedHeight;
            const drawWidth = Math.max(1, Math.round(displayWidth * scale));
            const drawHeight = Math.max(1, Math.round(displayHeight * scale));
            if (surface.width !== drawWidth)
                surface.width = drawWidth;
            if (surface.height !== drawHeight)
                surface.height = drawHeight;
            const uniforms = use(shader.source);
            gl.viewport(0, 0, drawWidth, drawHeight);
            if (!cachedImage || cachedImage.gl !== gl) {
                clearTexture();
                cachedImage = { gl, src: image.getAttribute("src"), texture: textureForImage(gl, image) };
            }
            gl.bindTexture(gl.TEXTURE_2D, cachedImage.texture);
            gl.uniform1i(uniforms.photo, 0);
            gl.uniform1i(uniforms.atlas, 1);
            const spanX = crop ? rect.width / fittedWidth : 1;
            const spanY = crop ? rect.height / fittedHeight : 1;
            gl.uniform4f(uniforms.crop, (1 - spanX) / 2, (1 - spanY) / 2, spanX, spanY);
            // One CSS inch is 96 CSS pixels. A 4:7 cell with this area maintains the density
            // when moving between displays with different framebuffer scales.
            const density = normalizeAsciiDensity(config.asciiDensity);
            const cellWidth = 96 * Math.sqrt((4 / 7) / density);
            const cellHeight = cellWidth * 7 / 4;
            const columns = Math.max(1, Math.round(displayWidth / cellWidth));
            const rows = Math.max(1, Math.round(displayHeight / cellHeight));
            gl.uniform2f(uniforms.cells, columns, rows);
            const glyphWidth = drawWidth / columns;
            gl.activeTexture(gl.TEXTURE1);
            gl.bindTexture(gl.TEXTURE_2D, atlases[glyphWidth < 6 ? 0 : glyphWidth < 12 ? 1 : 2]);
            gl.activeTexture(gl.TEXTURE0);
            gl.uniform1f(uniforms.seconds, motion.matches ? 0 : time / 1000);
            gl.uniform1i(uniforms.ascii, config.renderMode === "ascii" ? 1 : 0);
            gl.drawArrays(gl.TRIANGLE_STRIP, 0, 4);
            if (canvas.width !== width)
                canvas.width = width;
            if (canvas.height !== height)
                canvas.height = height;
            const context = canvas.getContext("2d");
            context.clearRect(0, 0, width, height);
            context.drawImage(surface, Math.round((width - drawWidth) / 2), Math.round((height - drawHeight) / 2));
            canvas.hidden = false;
            image.style.visibility = "hidden";
            lastError = "";
            if (shader.animated && !motion.matches)
                timer = setTimeout(schedule, 80);
        } catch (error) {
            showOriginal();
            if (lastError !== error.message) {
                lastError = error.message;
                onError(error);
            }
        }
    }

    function imageChanged() {
        clearTexture();
        showOriginal();
        schedule();
    }
    const observer = new IntersectionObserver(entries => {
        visible = entries[entries.length - 1].isIntersecting;
        if (!visible)
            clearTexture();
        schedule();
    });
    observer.observe(image);
    const resize = new ResizeObserver(schedule);
    resize.observe(image);
    image.addEventListener("load", imageChanged);
    document.addEventListener("visibilitychange", schedule);
    motion.addEventListener("change", schedule);

    return {
        update(next) {
            config = next;
            if (!image.complete || !image.naturalWidth || (cachedImage && cachedImage.src !== image.getAttribute("src"))) {
                clearTexture();
                showOriginal();
            }
            schedule();
        },
        dispose() {
            disposed = true;
            observer.disconnect();
            resize.disconnect();
            image.removeEventListener("load", imageChanged);
            document.removeEventListener("visibilitychange", schedule);
            motion.removeEventListener("change", schedule);
            stop();
            clearTexture();
            showOriginal();
        },
    };
}
