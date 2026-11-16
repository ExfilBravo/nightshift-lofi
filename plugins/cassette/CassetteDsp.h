#pragma once

#include <JuceHeader.h>
#include "common/LofiDsp.h"

/** DSP blocks for Nightshift Cassette. Everything is deterministic for a seed and
    allocates nothing after prepare(). Candidates for common/ once the other plugins
    want them: Svf (per-sample-safe shelves and bells), the normalised wander noise
    in Wobble, and the primeable one-pole.
*/
namespace cassette
{

constexpr float kPi = juce::MathConstants<float>::pi;
constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

//==============================================================================
/** Andrew Simper's trapezoidal state-variable filter. The coefficients can be
    recomputed every sample without clicks or instability, which the age filter
    (modulated by transients) and the smoothed shelves rely on. A shelf of +x dB
    followed by one of -x dB at the same frequency and Q cancels exactly, which the
    tape pre/de-emphasis pair depends on. */
class Svf
{
public:
    enum class Mode { lowpass, highpass, bell, lowShelf, highShelf };

    void prepare (double sampleRate)    { sr = sampleRate; reset(); }
    void reset()                        { ic1 = ic2 = 0.0f; }

    void set (Mode mode, float hz, float q, float gainDb = 0.0f)
    {
        hz = juce::jlimit (5.0f, (float) sr * 0.47f, hz);
        q = std::max (0.1f, q);
        const float A = std::pow (10.0f, gainDb / 40.0f);
        float g = std::tan (kPi * hz / (float) sr);
        float k = 1.0f / q;

        switch (mode)
        {
            case Mode::lowpass:   m0 = 0.0f;  m1 = 0.0f;                  m2 = 1.0f;         break;
            case Mode::highpass:  m0 = 1.0f;  m1 = -k;                    m2 = -1.0f;        break;
            case Mode::bell:      k = 1.0f / (q * A);
                                  m0 = 1.0f;  m1 = k * (A * A - 1.0f);    m2 = 0.0f;         break;
            case Mode::lowShelf:  g /= std::sqrt (A);
                                  m0 = 1.0f;  m1 = k * (A - 1.0f);        m2 = A * A - 1.0f; break;
            case Mode::highShelf: g *= std::sqrt (A);
                                  m0 = A * A; m1 = k * (1.0f - A) * A;    m2 = 1.0f - A * A; break;
        }

        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    float process (float v0) noexcept
    {
        const float v3 = v0 - ic2;
        const float v1 = a1 * ic1 + a2 * v3;
        const float v2 = ic2 + a2 * ic1 + a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        return m0 * v0 + m1 * v1 + m2 * v2;
    }

private:
    double sr = 48000.0;
    float a1 = 0.0f, a2 = 0.0f, a3 = 0.0f, m0 = 0.0f, m1 = 0.0f, m2 = 1.0f, ic1 = 0.0f, ic2 = 0.0f;
};

//==============================================================================
/** One-pole lowpass whose state can be primed, so it can be switched in mid-stream
    without a click. The dropout HF loss runs it continuously and crossfades. */
class OnePoleLp
{
public:
    void prepare (double sampleRate)    { sr = sampleRate; z = 0.0f; a = 1.0f; }
    void setCutoff (float hz)           { a = 1.0f - std::exp (-kTwoPi * juce::jlimit (10.0f, (float) sr * 0.49f, hz) / (float) sr); }
    void prime (float x)                { z = x; }
    float process (float x) noexcept    { z += (x - z) * a; return z; }

private:
    double sr = 48000.0;
    float a = 1.0f, z = 0.0f;
};

//==============================================================================
/** Tape transfer curve: a blend of a soft knee (x / sqrt (1 + x^2), ferric) and a
    harder one (tanh, metal). Both have unit slope at zero, so the clean level holds. */
inline float tapeCurve (float x, float blend) noexcept
{
    const float soft = x / std::sqrt (1.0f + x * x);
    const float hard = std::tanh (x);
    return soft + (hard - soft) * blend;
}

/** Amplitude of the fundamental that a sine of amplitude `amp` leaves after the biased
    curve (tapeCurve (x + bias) - tapeCurve (bias)). The drive compensation uses it to
    hold the loudness of a -12 dBFS signal as the drive rises. 64-point quadrature. */
inline float tapeFundamental (float amp, float blend, float bias)
{
    constexpr int n = 64;
    const float offset = tapeCurve (bias, blend);
    float sum = 0.0f;

    for (int i = 0; i < n; ++i)
    {
        const float s = std::sin (kTwoPi * ((float) i + 0.5f) / (float) n);
        sum += (tapeCurve (amp * s + bias, blend) - offset) * s;
    }

    return std::max (1.0e-6f, 2.0f * sum / (float) n);
}

//==============================================================================
/** Tape hiss: pink noise shaped into the familiar "shhh" (rising through the low
    mids, a broad presence hump, rolling off above). The level knob is calibrated to
    true dBFS RMS by measureRms() at prepare time. */
class Hiss
{
public:
    void prepare (double sampleRate, juce::uint32 seed)
    {
        noise.reset (seed);
        hp.prepare (sampleRate);
        lp.prepare (sampleRate);
        hp.set (Svf::Mode::highpass, 2500.0f, 0.6f);
        setTone (9000.0f);
    }

    void setTone (float lowpassHz)      { lp.set (Svf::Mode::lowpass, lowpassHz, 0.7f); }
    float next() noexcept               { return lp.process (hp.process (noise.pink())); }

    /** RMS of two seconds of hiss with this tone, as gain. Deterministic. */
    static float measureRms (double sampleRate, float lowpassHz)
    {
        Hiss h;
        h.prepare (sampleRate, 0xbeefcafeu);
        h.setTone (lowpassHz);
        const int n = (int) sampleRate * 2;
        double sum = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const float v = h.next();
            sum += (double) v * v;
        }

        return (float) std::sqrt (sum / (double) n);
    }

private:
    lofi::dsp::Noise noise;
    Svf hp, lp;
};

//==============================================================================
/** The tape transport's speed error, as two unit-ish signals: wow (hub and
    capstan eccentricity, 0.4-1.2 Hz) and flutter (6-12 Hz). Each is a few sines at
    unrelated rates whose rates themselves drift slowly, plus band-passed noise
    normalised to unit RMS, so it moves like a mechanism rather than an LFO.

    The noise is band-passed, not just low-passed: the speed error is integrated into
    a delay position, and noise with power down to DC would integrate into a slow
    random walk of the delay (image drift between L and R, and the position limiter
    eating the wow at high settings) instead of pitch movement. */
class Wobble
{
public:
    void prepare (double sampleRate, juce::uint32 seed)
    {
        sr = sampleRate;
        lofi::dsp::Noise rng (seed);

        for (auto& p : wowPhase)     p = rng.uniform();
        for (auto& p : flutterPhase) p = rng.uniform();
        for (auto& p : driftPhase)   p = rng.uniform();

        noise.reset (seed * 2654435761u + 7u);
        hw = onePole (0.25);  aw = onePole (0.7);     // wow noise: 0.25 .. 0.7 Hz
        hf = onePole (3.0);   af = onePole (22.0);    // flutter noise: 3 .. 22 Hz
        normW = bandNorm (hw, aw, 0.25);
        normF = bandNorm (hf, af, 3.0);
        wh = w1 = w2 = fh = f1 = f2 = 0.0;
    }

    /** The delay offset, in samples, whose derivative is this transport's current
        sine content at the given depths (cents): -A sr / (2 pi f) cos (phase) per
        line. Starting the position integrator here, rather than at 0, centres the
        wandering delay on the reported latency from the first sample after a reset. */
    double centredPosition (double wowCentsDepth, double flutterCentsDepth) const noexcept
    {
        constexpr double centsToSpeed = 5.776227e-4;   // ln 2 / 1200
        const double twoPi = juce::MathConstants<double>::twoPi;
        double p = 0.0;

        for (int k = 0; k < 3; ++k)
        {
            const double drift = std::sin (driftPhase[k] * twoPi);
            const double fw = wowRate[k] * (1.0 + 0.08 * drift);
            const double ff = flutterRate[k] * (1.0 + 0.05 * std::sin (driftPhase[(k + 1) % 3] * twoPi));
            p -= wowCentsDepth * centsToSpeed * wowWeight[k] * sr / (twoPi * fw) * std::cos (wowPhase[k] * twoPi);
            p -= flutterCentsDepth * centsToSpeed * flutterWeight[k] * sr / (twoPi * ff) * std::cos (flutterPhase[k] * twoPi);
        }

        return p;
    }

    void next (float& wow, float& flutter) noexcept
    {
        double drift[3];

        for (int k = 0; k < 3; ++k)
        {
            driftPhase[k] += driftRate[k] / sr;
            if (driftPhase[k] >= 1.0) driftPhase[k] -= 1.0;
            drift[k] = std::sin (driftPhase[k] * juce::MathConstants<double>::twoPi);
        }

        double w = 0.0, f = 0.0;

        for (int k = 0; k < 3; ++k)
        {
            wowPhase[k] += wowRate[k] * (1.0 + 0.08 * drift[k]) / sr;
            if (wowPhase[k] >= 1.0) wowPhase[k] -= 1.0;
            w += wowWeight[k] * std::sin (wowPhase[k] * juce::MathConstants<double>::twoPi);

            flutterPhase[k] += flutterRate[k] * (1.0 + 0.05 * drift[(k + 1) % 3]) / sr;
            if (flutterPhase[k] >= 1.0) flutterPhase[k] -= 1.0;
            f += flutterWeight[k] * std::sin (flutterPhase[k] * juce::MathConstants<double>::twoPi);
        }

        const double n = noise.white();
        wh += (n - wh) * hw;
        w1 += (n - wh - w1) * aw;  w2 += (w1 - w2) * aw;
        fh += (n - fh) * hf;
        f1 += (n - fh - f1) * af;  f2 += (f1 - f2) * af;

        wow = (float) (w + 0.15 * w2 * normW);
        flutter = (float) (f + 0.22 * f2 * normF);
    }

private:
    double onePole (double hz) const    { return 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * hz / sr); }

    /** Gain that brings uniform white noise through the band-pass (a one-pole
        high-pass, then two one-pole low-passes) to unit RMS: the energy of the chain's
        impulse response times the variance of the noise (1/3). Deterministic. */
    double bandNorm (double ah, double al, double lowestHz) const
    {
        const int n = (int) (sr * 2.5 / lowestHz);     // ten time constants of the slowest pole
        double h = 0.0, l1 = 0.0, l2 = 0.0, energy = 0.0;

        for (int i = 0; i < n; ++i)
        {
            const double x = i == 0 ? 1.0 : 0.0;
            h += (x - h) * ah;
            l1 += (x - h - l1) * al;
            l2 += (l1 - l2) * al;
            energy += l2 * l2;
        }

        return 1.0 / std::sqrt (std::max (1.0e-30, energy / 3.0));
    }

    static constexpr double wowRate[3]       { 0.62, 0.93, 0.41 };
    static constexpr double wowWeight[3]     { 0.55, 0.30, 0.15 };
    static constexpr double flutterRate[3]   { 6.9, 9.3, 11.6 };
    static constexpr double flutterWeight[3] { 0.50, 0.30, 0.20 };
    static constexpr double driftRate[3]     { 0.031, 0.047, 0.023 };

    double sr = 48000.0;
    double wowPhase[3] {}, flutterPhase[3] {}, driftPhase[3] {};
    double hw = 0.0, aw = 0.0, hf = 0.0, af = 0.0, normW = 1.0, normF = 1.0;
    double wh = 0.0, w1 = 0.0, w2 = 0.0, fh = 0.0, f1 = 0.0, f2 = 0.0;
    lofi::dsp::Noise noise;
};

//==============================================================================
/** Oxide shedding: brief, randomly spaced level dips with a raised-cosine shape,
    shared between the channels with slightly different depths.

    The wait for the next dip is kept as normalised progress (0 -> 1) rather than a
    sample countdown drawn when the gap starts, so turning the knob or automating it
    changes the rate on the next sample instead of after a gap of up to 18 s drawn
    at the old setting. */
class Dropouts
{
public:
    void prepare (double sampleRate, juce::uint32 seed)
    {
        sr = sampleRate;
        rng.reset (seed);
        progress = 0.0;
        invJitter = 1.0 / (0.5 + rng.uniform());
        remaining = 0;
        length = 1;
        depth[0] = depth[1] = 0.0f;
        setAmount (0.0f);
    }

    /** amount 0..1, read once per block. */
    void setAmount (float a) noexcept
    {
        amount = a;
        const double mean = 0.25 + 12.0 * (1.0 - a) * (1.0 - a);    // seconds between dips
        rate = 1.0 / (sr * mean);
    }

    /** Ends any dip in progress (after a reset the wet path restarts clean). */
    void cancel() noexcept      { remaining = 0; }

    /** Dips are 0 (untouched) .. 1 (silent). */
    void next (float& dipL, float& dipR) noexcept
    {
        if (remaining > 0)
        {
            const float t = 1.0f - (float) remaining / (float) length;
            const float w = 0.5f - 0.5f * std::cos (kTwoPi * t);
            dipL = w * depth[0];
            dipR = w * depth[1];
            --remaining;
            return;
        }

        dipL = dipR = 0.0f;

        if (amount < 0.001f)
            return;

        // Each gap is the mean times a jitter of 0.5 .. 1.5, drawn when the gap starts
        progress += rate * invJitter;

        if (progress >= 1.0)
        {
            progress = 0.0;
            invJitter = 1.0 / (0.5 + rng.uniform());
            start();
        }
    }

private:
    void start()
    {
        const float u = rng.uniform();
        length = std::max (1, (int) (sr * 0.001 * (30.0 + 170.0 * std::pow (u, 1.5f))));
        remaining = length;
        const float depthDb = 3.0f + 15.0f * amount * (0.3f + 0.7f * rng.uniform());
        const float d = 1.0f - std::pow (10.0f, -depthDb / 20.0f);
        depth[0] = d * (0.8f + 0.2f * rng.uniform());
        depth[1] = d * (0.8f + 0.2f * rng.uniform());
    }

    double sr = 48000.0, progress = 0.0, invJitter = 1.0, rate = 0.0;
    lofi::dsp::Noise rng;
    int remaining = 0, length = 1;
    float amount = 0.0f, depth[2] {};
};

} // namespace cassette
