// 24-bit stereo WAV with a BWF `bext` chunk carrying the start timecode,
// so an NLE can place the stem at the right point of the timeline.
// The header holds no date, so the same render gives a byte-identical file.
//
// SN7 -- docs/roadmap/SON-2026-10.md. Plain typed arrays, no Node Buffer: the JavaScriptCore host
// (cutmachine-sound-host) encodes with this same function, so both engines write the same bytes.

/**
 * @returns {{ bytes: Uint8Array, dataOffset: number, clipped: number, peak: number }}
 *          the whole file, and where its sample data starts (what the report's sha256 covers)
 */
export function encodeWav({ left, right, sampleRate, timeReference = 0, description = '' }) {
    const n = left.length;
    const BEXT = 602, FMT = 16, DATA = n * 6;
    const size = 12 + (8 + BEXT) + (8 + FMT) + (8 + DATA);
    const bytes = new Uint8Array(size);
    const view = new DataView(bytes.buffer);
    let o = 0;
    // As Buffer.write(..., 'ascii') did: one byte per UTF-16 unit, its low eight bits.
    const text = (s, at, len) => { for (let i = 0; i < Math.min(s.length, len); i++) bytes[at + i] = s.charCodeAt(i) & 0xff; };
    const str = (s, len) => { text(s, o, len); o += len; };
    const u32 = (v) => { view.setUint32(o, v >>> 0, true); o += 4; };
    const u16 = (v) => { view.setUint16(o, v, true); o += 2; };

    str('RIFF', 4); u32(size - 8); str('WAVE', 4);

    str('bext', 4); u32(BEXT);
    const bext = o;
    text(description, bext, 256);                                    // Description
    text('cutmachine-sound', bext + 256, 32);                        // Originator
    const tr = Math.max(0, Math.round(timeReference));
    view.setUint32(bext + 338, tr % 0x100000000, true);              // TimeReferenceLow
    view.setUint32(bext + 342, Math.floor(tr / 0x100000000), true);  // TimeReferenceHigh
    view.setUint16(bext + 346, 1, true);                             // Version
    o = bext + BEXT;

    str('fmt ', 4); u32(FMT);
    u16(1); u16(2); u32(sampleRate); u32(sampleRate * 6); u16(6); u16(24);

    str('data', 4); u32(DATA);
    const dataOffset = o;
    let clipped = 0, peak = 0;
    for (let i = 0; i < n; i++) {
        for (let c = 0; c < 2; c++) {
            let x = c === 0 ? left[i] : right[i];
            const a = Math.abs(x);
            if (a > peak) peak = a;
            if (a > 1) { clipped++; x = x > 0 ? 1 : -1; }
            const v = Math.max(-8388608, Math.min(8388607, Math.round(x * 8388607)));
            bytes[o] = v & 0xff;
            bytes[o + 1] = (v >> 8) & 0xff;
            bytes[o + 2] = (v >> 16) & 0xff;
            o += 3;
        }
    }
    return { bytes, dataOffset, clipped, peak };
}
