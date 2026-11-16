// Audio analysis for checking renders by measurement.
// Usage: node tools/analyze.mjs <file.wav|folder> [--onsets] [--json]
// Reports peak, RMS, integrated loudness (BS.1770), spectral balance, low-end pitch,
// decay time and onsets, so sound claims can be backed by numbers.

import fs from 'node:fs';
import path from 'node:path';

const args = process.argv.slice(2);
const showOnsets = args.includes('--onsets');
const asJson = args.includes('--json');
const targets = args.filter(a => !a.startsWith('--'));

function readWav(file) {
  const buf = fs.readFileSync(file);
  if (buf.toString('ascii', 0, 4) !== 'RIFF' || buf.toString('ascii', 8, 12) !== 'WAVE')
    throw new Error('not a RIFF/WAVE file');

  let fmt = null, dataOffset = -1, dataSize = 0, pos = 12;

  while (pos + 8 <= buf.length) {
    const id = buf.toString('ascii', pos, pos + 4);
    const size = buf.readUInt32LE(pos + 4);
    if (id === 'fmt ') {
      fmt = {
        format: buf.readUInt16LE(pos + 8),
        channels: buf.readUInt16LE(pos + 10),
        sampleRate: buf.readUInt32LE(pos + 12),
        bits: buf.readUInt16LE(pos + 22),
      };
      if (fmt.format === 0xFFFE) fmt.format = buf.readUInt16LE(pos + 32);   // WAVE_FORMAT_EXTENSIBLE
    } else if (id === 'data') {
      dataOffset = pos + 8;
      dataSize = Math.min(size, buf.length - dataOffset);
    }
    pos += 8 + size + (size & 1);
  }

  if (!fmt || dataOffset < 0) throw new Error('missing fmt or data chunk');

  const bytes = fmt.bits / 8;
  const frames = Math.floor(dataSize / (bytes * fmt.channels));
  const channels = Array.from({ length: fmt.channels }, () => new Float32Array(frames));

  for (let i = 0; i < frames; i++) {
    for (let c = 0; c < fmt.channels; c++) {
      const o = dataOffset + (i * fmt.channels + c) * bytes;
      let v;
      if (fmt.format === 3) v = bytes === 4 ? buf.readFloatLE(o) : buf.readDoubleLE(o);
      else if (bytes === 2) v = buf.readInt16LE(o) / 32768;
      else if (bytes === 3) v = (buf.readIntLE(o, 3)) / 8388608;
      else if (bytes === 4) v = buf.readInt32LE(o) / 2147483648;
      else v = (buf[o] - 128) / 128;
      channels[c][i] = v;
    }
  }

  return { sampleRate: fmt.sampleRate, bits: fmt.bits, channels, frames };
}

const db = g => (g > 0 ? 20 * Math.log10(g) : -Infinity);
const fmtDb = v => (Number.isFinite(v) ? v.toFixed(1) : '-inf');

function biquad(x, b0, b1, b2, a1, a2) {
  const y = new Float32Array(x.length);
  let x1 = 0, x2 = 0, y1 = 0, y2 = 0;
  for (let i = 0; i < x.length; i++) {
    const v = b0 * x[i] + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
    x2 = x1; x1 = x[i]; y2 = y1; y1 = v; y[i] = v;
  }
  return y;
}

// ITU-R BS.1770 K-weighting, coefficients derived for any sample rate
function kWeight(x, fs) {
  let f0 = 1681.974450955533, G = 3.999843853973347, Q = 0.7071752369554196;
  let K = Math.tan(Math.PI * f0 / fs);
  const Vh = Math.pow(10, G / 20), Vb = Math.pow(Vh, 0.4996667741545416);
  let a0 = 1 + K / Q + K * K;
  const s1 = biquad(x, (Vh + Vb * K / Q + K * K) / a0, 2 * (K * K - Vh) / a0, (Vh - Vb * K / Q + K * K) / a0,
                    2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0);
  f0 = 38.13547087602444; Q = 0.5003270373238773;
  K = Math.tan(Math.PI * f0 / fs);
  a0 = 1 + K / Q + K * K;
  return biquad(s1, 1, -2, 1, 2 * (K * K - 1) / a0, (1 - K / Q + K * K) / a0);
}

function integratedLoudness(channels, fs) {
  const weighted = channels.map(c => kWeight(c, fs));
  const block = Math.round(0.4 * fs), hop = Math.round(0.1 * fs);
  const blocks = [];
  for (let s = 0; s + block <= weighted[0].length; s += hop) {
    let sum = 0;
    for (const w of weighted) { let ms = 0; for (let i = s; i < s + block; i++) ms += w[i] * w[i]; sum += ms / block; }
    blocks.push(sum);
  }
  if (!blocks.length) return -Infinity;
  const lufs = ms => -0.691 + 10 * Math.log10(ms);
  const abs = blocks.filter(b => lufs(b) > -70);
  if (!abs.length) return -Infinity;
  const relGate = lufs(abs.reduce((a, b) => a + b, 0) / abs.length) - 10;
  const rel = abs.filter(b => lufs(b) > relGate);
  return lufs(rel.reduce((a, b) => a + b, 0) / rel.length);
}

function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i++) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { [re[i], re[j]] = [re[j], re[i]]; [im[i], im[j]] = [im[j], im[i]]; }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = -2 * Math.PI / len, wr = Math.cos(ang), wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cr = 1, ci = 0;
      for (let k = 0; k < len / 2; k++) {
        const ur = re[i + k], ui = im[i + k];
        const vr = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
        const vi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
        re[i + k] = ur + vr; im[i + k] = ui + vi;
        re[i + k + len / 2] = ur - vr; im[i + k + len / 2] = ui - vi;
        const t = cr * wr - ci * wi; ci = cr * wi + ci * wr; cr = t;
      }
    }
  }
}

// Average power spectrum over Hann-windowed frames of the mono mix
function powerSpectrum(mono, n = 8192) {
  const spec = new Float64Array(n / 2);
  const win = Float64Array.from({ length: n }, (_, i) => 0.5 - 0.5 * Math.cos(2 * Math.PI * i / (n - 1)));
  let frames = 0;
  for (let s = 0; s + n <= Math.max(mono.length, n); s += n / 2) {
    const re = new Float64Array(n), im = new Float64Array(n);
    for (let i = 0; i < n; i++) re[i] = (mono[s + i] || 0) * win[i];
    fft(re, im);
    for (let k = 0; k < n / 2; k++) spec[k] += re[k] * re[k] + im[k] * im[k];
    frames++;
  }
  for (let k = 0; k < n / 2; k++) spec[k] /= frames;
  return spec;
}

const bands = [['sub', 20, 60], ['bass', 60, 250], ['lowmid', 250, 1000], ['mid', 1000, 4000], ['high', 4000, 10000], ['air', 10000, 24000]];

function analyse(file) {
  const { sampleRate: fs, bits, channels, frames } = readWav(file);
  const mono = new Float32Array(frames);
  for (const c of channels) for (let i = 0; i < frames; i++) mono[i] += c[i] / channels.length;

  let peak = 0, sumSq = 0, dc = 0;
  for (const c of channels) for (let i = 0; i < frames; i++) { const a = Math.abs(c[i]); if (a > peak) peak = a; sumSq += c[i] * c[i]; dc += c[i]; }
  const rms = Math.sqrt(sumSq / (frames * channels.length));

  const n = 8192, spec = powerSpectrum(mono, n), binHz = fs / n;
  let total = 0, centroidNum = 0;
  for (let k = 1; k < spec.length; k++) { total += spec[k]; centroidNum += spec[k] * k * binHz; }
  const bandShare = Object.fromEntries(bands.map(([name, lo, hi]) => {
    let e = 0;
    for (let k = Math.ceil(lo / binHz); k < Math.min(spec.length, hi / binHz); k++) e += spec[k];
    return [name, +(100 * e / total).toFixed(1)];
  }));

  // Strongest low partial (kicks, 808s, basslines)
  let lowPeakBin = 0;
  for (let k = Math.ceil(25 / binHz); k < 250 / binHz; k++) if (spec[k] > spec[lowPeakBin]) lowPeakBin = k;
  let lowPeakHz = lowPeakBin * binHz;
  if (lowPeakBin > 0 && lowPeakBin < spec.length - 1) {   // parabolic interpolation
    const a = Math.log(spec[lowPeakBin - 1] + 1e-20), b = Math.log(spec[lowPeakBin] + 1e-20), c = Math.log(spec[lowPeakBin + 1] + 1e-20);
    lowPeakHz = (lowPeakBin + 0.5 * (a - c) / (a - 2 * b + c)) * binHz;
  }
  const midi = 69 + 12 * Math.log2(lowPeakHz / 440);
  const names = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];
  const nearest = Math.round(midi);

  // 5 ms envelope for decay and onsets
  const hop = Math.max(1, Math.round(fs * 0.005));
  const env = [];
  for (let s = 0; s < frames; s += hop) { let m = 0; for (let i = s; i < Math.min(frames, s + hop); i++) m = Math.max(m, Math.abs(mono[i])); env.push(m); }
  const peakIdx = env.indexOf(Math.max(...env));
  let decayIdx = env.length - 1;
  for (let i = peakIdx; i < env.length; i++) if (db(env[i]) < db(env[peakIdx]) - 40) { decayIdx = i; break; }

  const onsets = [];
  for (let i = 1; i < env.length; i++) {
    const rise = db(env[i] + 1e-9) - db(env[i - 1] + 1e-9);
    if (rise > 9 && db(env[i]) > db(peak) - 30 && (!onsets.length || (i * hop / fs) - onsets[onsets.length - 1] > 0.05))
      onsets.push(+(i * hop / fs).toFixed(3));
  }

  let clipped = 0;
  for (const c of channels) for (let i = 0; i < frames; i++) if (Math.abs(c[i]) >= 0.999) clipped++;

  return {
    file: path.basename(file), sampleRate: fs, bits, channels: channels.length,
    seconds: +(frames / fs).toFixed(3),
    peakDb: +db(peak).toFixed(2), rmsDb: +db(rms).toFixed(2), crestDb: +(db(peak) - db(rms)).toFixed(1),
    lufs: +integratedLoudness(channels, fs).toFixed(1),
    dc: +(dc / (frames * channels.length)).toExponential(1), clippedSamples: clipped,
    centroidHz: Math.round(centroidNum / total), bands: bandShare,
    lowPeak: `${lowPeakHz.toFixed(1)} Hz (${names[((nearest % 12) + 12) % 12]}${Math.floor(nearest / 12) - 1}, ${((midi - nearest) * 100).toFixed(0)} cents)`,
    decayTo40dB: +((decayIdx - peakIdx) * hop / fs).toFixed(3),
    onsets: onsets.length, onsetTimes: showOnsets ? onsets : undefined,
  };
}

const files = targets.flatMap(t => fs.statSync(t).isDirectory()
  ? fs.readdirSync(t).filter(f => f.toLowerCase().endsWith('.wav')).map(f => path.join(t, f))
  : [t]);

for (const f of files) {
  try {
    const r = analyse(f);
    if (asJson) { console.log(JSON.stringify(r)); continue; }
    console.log(`${r.file}  ${r.seconds}s ${r.sampleRate}Hz/${r.bits}bit ${r.channels}ch`);
    console.log(`  peak ${fmtDb(r.peakDb)} dBFS  rms ${fmtDb(r.rmsDb)}  crest ${r.crestDb} dB  loudness ${fmtDb(r.lufs)} LUFS  clipped ${r.clippedSamples}  dc ${r.dc}`);
    console.log(`  centroid ${r.centroidHz} Hz  bands% ${Object.entries(r.bands).map(([k, v]) => `${k} ${v}`).join(' | ')}`);
    console.log(`  low peak ${r.lowPeak}  decay to -40 dB ${r.decayTo40dB}s  onsets ${r.onsets}${showOnsets ? ' ' + JSON.stringify(r.onsetTimes) : ''}`);
  } catch (e) {
    console.log(`${path.basename(f)}: ${e.message}`);
  }
}
