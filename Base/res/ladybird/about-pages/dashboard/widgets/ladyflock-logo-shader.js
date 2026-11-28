/*
 * Copyright (c) 2026-present, the Ladyflock developers.
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The shader for the Ladyflock lockup (emblem on the left, wordmark on the right).
// It is one loop, sixteen seconds long, that never stops. The emblem's rings turn: the light
// and dark ends of their gradient travel once round the emblem. The three birds are lifted out
// of the picture and drawn again where they have flown to, rising and falling on a swell and
// beating their wings. The pointer changes one thing, the flock's heading: the birds climb,
// dive or press on towards it, the leader first. The wordmark stays still, written in the
// page's text colour on a dark page, where its own near-black ink is lost.
//
// Positions are in units of the image's height, x included, and were measured on the lockup.

const source = `
    uniform vec3 ink;
    uniform float aspect;
    uniform vec2 lead;
    uniform vec2 middle;
    uniform vec2 trail;

    const float TURN = 6.2831853;

    float luma(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

    vec4 sampleAt(vec2 q) { return sampleImage(vec2(q.x / aspect, q.y)); }

    float inBox(vec2 q, vec4 box) {
        return step(box.x, q.x) * step(q.x, box.y) * step(box.z, q.y) * step(q.y, box.w);
    }

    float beyond(vec2 q, vec3 line) { return step(line.z, dot(q, line.xy)); }

    // Where each bird is in the picture: a box, cut by the lines that keep its neighbours
    // and the rings out. Nothing but clear space lies between a bird and the edge of its region.
    float leadBird(vec2 q) {
        return inBox(q, vec4(0.6387, 0.8730, 0.3262, 0.4688))
            * beyond(q, vec3(-0.5299, -0.8480, -0.8027)) * beyond(q, vec3(0.8660, -0.5, 0.3713));
    }

    float middleBird(vec2 q) {
        return inBox(q, vec4(0.5176, 0.6836, 0.4023, 0.5117)) * beyond(q, vec3(-0.4067, -0.9135, -0.7047))
            * beyond(q, vec3(0.9336, -0.3584, 0.3275)) * beyond(q, vec3(-0.8660, 0.5, -0.3713));
    }

    float trailBird(vec2 q) {
        return inBox(q, vec4(0.4160, 0.5430, 0.4707, 0.5566)) * beyond(q, vec3(-0.9336, 0.3584, -0.3275));
    }

    // The place in the picture that a bird's flight brings to q. The bird turns about its
    // shoulder, and only what is above the shoulder, the wing, stretches with the wingbeat.
    vec2 flown(vec2 q, vec2 shoulder, vec2 heading, float lag, float phase) {
        vec2 drift = vec2(0.006 * sin(TURN * (2.0 * phase - lag)), 0.011 * sin(TURN * (3.0 * phase - lag)))
            + heading * 0.012;
        float pitch = 0.05 * cos(TURN * (3.0 * phase - lag)) - heading.y * 0.2;
        float beat = 1.0 + 0.07 * sin(TURN * (8.0 * phase - 2.0 * lag));
        vec2 from = q - shoulder - drift;
        vec2 turned = vec2(cos(pitch) * from.x - sin(pitch) * from.y, sin(pitch) * from.x + cos(pitch) * from.y);
        turned.y /= mix(1.0, beat, step(turned.y, 0.0));
        return shoulder + turned;
    }

    vec4 over(vec4 top, vec4 under) {
        float alpha = top.a + under.a * (1.0 - top.a);
        return vec4(mix(under.rgb, top.rgb, top.a / max(alpha, 0.0001)), alpha);
    }

    vec4 shade(vec2 uv, vec4 color, float time) {
        // The lockup is a cutout: its corners are clear. On an image that fills its frame, such as
        // a photograph, nothing is the flock or the wordmark, and only a faint turning tint is left.
        float cutout = 1.0 - max(max(sampleImage(vec2(0.0, 0.0)).a, sampleImage(vec2(1.0, 0.0)).a),
            max(sampleImage(vec2(0.0, 1.0)).a, sampleImage(vec2(1.0, 1.0)).a));
        float phase = fract(time / 16.0);
        float night = smoothstep(0.4, 0.6, luma(ink));
        vec2 q = vec2(uv.x * aspect, uv.y);

        // The birds leave the picture here and are drawn again below.
        vec4 texel = color;
        texel.a *= 1.0 - cutout * max(leadBird(q), max(middleBird(q), trailBird(q)));

        // The emblem is saturated violet. The wordmark is nearly black, with little colour of its own,
        // and so are the shadows where one ring passes under the other, which are not lettering.
        float colour = max(texel.r, max(texel.g, texel.b)) - min(texel.r, min(texel.g, texel.b));
        float wordmark = (1.0 - smoothstep(0.18, 0.34, colour)) * (1.0 - smoothstep(0.2, 0.4, luma(texel.rgb)))
            * smoothstep(0.33, 0.36, uv.x) * cutout;

        // Both rings go once round the point where they overlap, so the angle about that point
        // runs along each of them, and a wave in that angle is a wave travelling along the rings.
        float around = atan(q.y - 0.629, q.x - 0.656) / TURN;
        float wave = cos(TURN * (around - phase));
        float crest = smoothstep(-0.2, 1.0, wave) * (0.82 + 0.18 * cos(TURN * (2.0 * around + phase)));
        float trough = 1.0 - smoothstep(-1.0, 0.1, wave);
        float turning = mix(0.3, 1.0, cutout);
        vec3 rings = mix(texel.rgb, vec3(0.80, 0.60, 1.0), crest * 0.34 * turning);
        rings *= 1.0 - trough * 0.2 * turning * vec3(1.0, 1.0, 0.55);

        vec4 lit = vec4(mix(rings, mix(texel.rgb, ink, night), wordmark), texel.a);

        vec2 from = flown(q, vec2(0.489, 0.523), trail, 0.22, phase);
        vec4 bird = sampleAt(from);
        bird.a *= trailBird(from);
        from = flown(q, vec2(0.613, 0.470), middle, 0.11, phase);
        vec4 next = sampleAt(from);
        next.a *= middleBird(from);
        bird = over(next, bird);
        from = flown(q, vec2(0.775, 0.415), lead, 0.0, phase);
        next = sampleAt(from);
        next.a *= leadBird(from);
        bird = over(next, bird);
        // The flock's own deep violet is hard to see on a dark page.
        bird.rgb = mix(bird.rgb, vec3(0.62, 0.45, 0.95), night * 0.3);
        bird.a *= cutout;
        return over(bird, lit);
    }`;

const ASPECT = 3;
// The middle of the flock, as a fraction of the image's width and of its height.
const FLOCK = [0.2, 0.455];
// How quickly each bird takes up a new heading, per second: leader, middle, last.
const PACE = [5, 3.2, 2.2];
const FRAME_MS = 60;

function smoothstep(from, to, value) {
    const t = Math.max(0, Math.min(1, (value - from) / (to - from)));
    return t * t * (3 - 2 * t);
}

// The heading the pointer asks for: towards it from the flock, at most one unit long. The flock
// heeds a pointer on the image or a little way off it, and less and less the further off it is.
function headingFor(pointer) {
    if (!pointer)
        return [0, 0];
    const x = (pointer[0] - FLOCK[0]) * ASPECT;
    const y = pointer[1] - FLOCK[1];
    const far = Math.hypot(x, y);
    const off = Math.hypot(Math.max(0, -pointer[0], pointer[0] - 1) * ASPECT, Math.max(0, -pointer[1], pointer[1] - 1));
    const pull = (1 - smoothstep(0.25, 1.1, off)) / (far + 0.35);
    return [x * pull, y * pull];
}

function createState() {
    let previous = null;
    const headings = PACE.map(() => [0, 0]);

    return {
        // `pointer` is the pointer's place in image space (0-1 on both axes), or null when it is
        // not over the page. The answer is the uniforms for the frame: each bird's heading.
        frame({ seconds, pointer, still }) {
            const wanted = still ? [0, 0] : headingFor(pointer);
            const step = Math.min(0.1, previous === null ? 0 : seconds - previous);
            previous = seconds;
            headings.forEach((heading, bird) => {
                const gain = still ? 1 : 1 - Math.exp(-step * PACE[bird]);
                heading[0] += (wanted[0] - heading[0]) * gain;
                heading[1] += (wanted[1] - heading[1]) * gain;
            });
            return { lead: headings[0], middle: headings[1], trail: headings[2] };
        },
    };
}

export const ladyflockLogoShader = { source, animated: true, frameMs: FRAME_MS, fullColor: true, createState };
