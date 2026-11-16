#pragma once

#include <JuceHeader.h>
#include "common/LofiDsp.h"

/** DSP blocks for Nightshift Vinyl. Everything is deterministic for a seed and
    allocates nothing after prepare(). Svf is the same trapezoidal SVF Cassette
    carries; common/ is frozen while four plugins are built side by side, so it lives
    here too and is a candidate for common/LofiDsp.h.
*/
namespace vinyl
{

constexpr float kPi = juce::MathConstants<float>::pi;
constexpr float kTwoPi = juce::MathConstants<float>::twoPi;

/** A bounded ceiling: x / (1 + (x/k)^8)^(1/8). Leaves anything under about 0.7 k
    untouched (0.02 dB at 0.6 k), never exceeds k. */
inline float ceiling (float x, float k) noexcept
{
    const float u2 = (x / k) * (x / k);
    const float u8 = u2 * u2 * u2 * u2;
    return x / std::sqrt (std::sqrt (std::sqrt (1.0f + u8)));
}

//==============================================================================
/** Andrew Simper's trapezoidal state-variable filter: coefficients can change
    every sample without clicks, which the wear EQ relies on while its knob moves. */
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
/** Surface static: pink noise band-limited to 500 Hz - 8 kHz, then a soft ceiling
    at about 2.2 sigma so the bed never throws peaks of its own (a record's static
    is a texture; the pops are the only things that should poke out of it). The
    two gains from calibrate() make next() unit RMS, so the level knob is true dBFS. */
class Static
{
public:
    struct Norms { float pre = 1.0f, post = 1.0f; };

    void prepare (double sampleRate, juce::uint32 seed)
    {
        noise.reset (seed);
        hp.prepare (sampleRate);
        lp.prepare (sampleRate);
        hp.set (Svf::Mode::highpass, 500.0f, 0.7f);
        lp.set (Svf::Mode::lowpass, 8000.0f, 0.7f);
    }

    void setNorms (Norms n)             { norms = n; }

    float next() noexcept
    {
        return limit (raw() * norms.pre) * norms.post;
    }

    /** Deterministic: a second of static at this rate, measured before and after the ceiling. */
    static Norms calibrate (double sampleRate)
    {
        Norms n;
        Static s;
        const int count = (int) sampleRate;

        s.prepare (sampleRate, 0xfacade01u);
        double sum = 0.0;

        for (int i = 0; i < count; ++i)
        {
            const float v = s.raw();
            sum += (double) v * v;
        }

        n.pre = 1.0f / std::max (1.0e-9f, (float) std::sqrt (sum / count));

        s.prepare (sampleRate, 0xfacade02u);
        sum = 0.0;

        for (int i = 0; i < count; ++i)
        {
            const float v = limit (s.raw() * n.pre);
            sum += (double) v * v;
        }

        n.post = 1.0f / std::max (1.0e-9f, (float) std::sqrt (sum / count));
        return n;
    }

private:
    static constexpr float knee = 2.2f;

    float raw() noexcept                { return lp.process (hp.process (noise.pink())); }
    static float limit (float x) noexcept { return knee * lofi::dsp::shape::tanhFast (x * (1.0f / knee)); }

    lofi::dsp::Noise noise;
    Svf hp, lp;
    Norms norms;
};

//==============================================================================
/** Crackle: Poisson-spaced pops (a damped 1-4 kHz click over a lower thump, 1-4 ms,
    random polarity, the loudest exactly at the peak gain asked for) plus a bed of
    much finer, denser ticks between them. Pops land near the centre with a little
    left/right imbalance; ticks scatter across the stage. */
class Crackle
{
public:
    void prepare (double sampleRate, juce::uint32 seed)
    {
        sr = sampleRate;
        rng.reset (seed);

        for (auto& v : voices)
            v = {};

        popRate = tickRate = 0.0f;
        popCountdown = tickCountdown = -1;
        tickSlot = 0;
    }

    /** Ends any pop or tick in flight; the schedule carries on. */
    void silence() noexcept
    {
        for (auto& v : voices)
            v.remaining = 0;
    }

    /** Mean events per second; 0 switches a stream off. */
    void setRates (float popsPerSecond, float ticksPerSecond)
    {
        reschedule (popCountdown, popRate, popsPerSecond, popGap);
        reschedule (tickCountdown, tickRate, ticksPerSecond, tickGap);
    }

    /** popPeak / tickPeak: peak gain of the loudest event; spread 0 = mono, 1 = full stereo. */
    void next (float& l, float& r, float popPeak, float tickPeak, float spread) noexcept
    {
        l = r = 0.0f;

        if (popRate > 0.0f && --popCountdown <= 0)
        {
            startPop (voices[0], popPeak, spread);
            popCountdown = interval (popRate, popGap);
        }

        if (tickRate > 0.0f && --tickCountdown <= 0)
        {
            startTick (voices[1 + tickSlot], tickPeak, spread);
            tickSlot ^= 1;
            tickCountdown = interval (tickRate, tickGap);
        }

        for (auto& v : voices)
        {
            if (v.remaining <= 0)
                continue;

            float x = v.env * std::cos (v.phase) + v.lowMix * v.env2 * std::cos (v.phase2);
            v.env *= v.decay;
            v.env2 *= v.decay2;
            v.phase += v.inc;
            v.phase2 += v.inc2;

            if (v.remaining < v.fade)
                x *= (float) v.remaining / (float) v.fade;

            --v.remaining;
            l += x * v.gainL;
            r += x * v.gainR;
        }
    }

private:
    struct Voice
    {
        int remaining = 0, fade = 1;
        float phase = 0.0f, inc = 0.0f, env = 1.0f, decay = 1.0f;
        float phase2 = 0.0f, inc2 = 0.0f, env2 = 1.0f, decay2 = 1.0f;
        float lowMix = 0.0f, gainL = 0.0f, gainR = 0.0f;
    };

    static constexpr double popGap = 0.03, tickGap = 0.002;   // seconds, minimum spacing

    /** Dust along a groove is spread fairly evenly, so the wait is 0.3 .. 1.7 times the
        mean rather than exponential: close pairs still happen, long silences do not. */
    int interval (float rate, double gap)
    {
        const double s = (0.3 + 1.4 * rng.uniform()) / (double) rate;
        return std::max ((int) (gap * sr), (int) (s * sr));
    }

    void reschedule (int& countdown, float& rate, float newRate, double gap)
    {
        if (std::abs (newRate - rate) < 1.0e-6f)
            return;

        if (newRate <= 0.0f)
        {
            countdown = -1;
            rate = 0.0f;
            return;
        }

        if (rate <= 0.0f || countdown < 0)
        {
            rate = newRate;
            countdown = interval (newRate, gap);
            return;
        }

        // Poisson arrivals are memoryless: stretch what is left of the wait by the rate change
        countdown = std::max (1, (int) ((double) countdown * rate / newRate));
        rate = newRate;
    }

    void start (Voice& v, float f1, float tau1, float f2, float tau2, float lowMix, float lengthMs, float amp, float gl, float gr)
    {
        v.remaining = std::max (8, (int) (lengthMs * 0.001 * sr));
        v.fade = std::max (2, v.remaining / 3);
        v.phase = v.phase2 = 0.0f;
        v.inc = kTwoPi * f1 / (float) sr;
        v.inc2 = kTwoPi * f2 / (float) sr;
        v.env = v.env2 = 1.0f;
        v.decay = std::exp (-1.0f / (tau1 * (float) sr));
        v.decay2 = std::exp (-1.0f / (tau2 * (float) sr));
        v.lowMix = lowMix;

        // Both partials start at their crest, so the first sample is the peak: amp exactly
        const float norm = amp / ((1.0f + lowMix) * std::max (gl, gr));
        v.gainL = gl * norm;
        v.gainR = gr * norm;
    }

    void startPop (Voice& v, float peak, float spread)
    {
        const float f1 = 1200.0f + 3300.0f * rng.uniform();
        const float tau1 = 0.00025f + 0.00065f * rng.uniform();
        const float f2 = 250.0f + 550.0f * rng.uniform();
        const float tau2 = 0.001f + 0.002f * rng.uniform();
        const float lowMix = 0.2f + 0.3f * rng.uniform();
        const float lengthMs = 1.0f + 3.0f * rng.uniform();
        const float amp = peak * lofi::dsp::dbToGain (-2.0f * rng.uniform()) * (rng.uniform() < 0.5f ? -1.0f : 1.0f);
        const float side = spread * 0.1f * (rng.uniform() - 0.5f);
        start (v, f1, tau1, f2, tau2, lowMix, lengthMs, amp, 1.0f + side, 1.0f - side);
    }

    void startTick (Voice& v, float peak, float spread)
    {
        const float f1 = 3000.0f + 6000.0f * rng.uniform();
        const float tau1 = 0.00008f + 0.00017f * rng.uniform();
        const float lengthMs = 0.3f + 0.7f * rng.uniform();
        const float amp = peak * lofi::dsp::dbToGain (-10.0f * rng.uniform()) * (rng.uniform() < 0.5f ? -1.0f : 1.0f);
        const float pan = 0.5f + spread * 0.9f * (rng.uniform() - 0.5f);
        start (v, f1, tau1, f1, tau1, 0.0f, lengthMs, amp, std::cos (pan * kPi * 0.5f), std::sin (pan * kPi * 0.5f));
    }

    double sr = 48000.0;
    lofi::dsp::Noise rng;
    Voice voices[3];
    float popRate = 0.0f, tickRate = 0.0f;
    int popCountdown = -1, tickCountdown = -1, tickSlot = 0;
};

//==============================================================================
/** The platter's speed error. A warped or off-centre record bends pitch once per
    revolution: a fundamental at the platter rate with a weaker second and a faint
    third harmonic (a warp is a bump, not a sine), the depth breathing slowly as the
    stylus works inward, plus a trace of band-limited jitter from stylus drag. */
class Warp
{
public:
    void prepare (double sampleRate, juce::uint32 seed)
    {
        sr = sampleRate;
        lofi::dsp::Noise rng (seed);
        phase = rng.uniform();
        theta2 = rng.uniform() * kTwoPi;
        theta3 = rng.uniform() * kTwoPi;

        float peak = 0.0f;

        for (int i = 0; i < 720; ++i)
            peak = std::max (peak, std::abs (shapeAt ((float) i / 720.0f)));

        norm = 1.0f / std::max (1.0e-3f, peak);

        noise.reset (seed * 2654435761u + 11u);
        aSlow = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 0.07 / sr);
        aJit = 1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * 4.0 / sr);
        normSlow = twoPoleNorm (aSlow);
        normJit = twoPoleNorm (aJit);
        s1 = s2 = j1 = j2 = 0.0;
    }

    /** Speed error as (ratio - 1) for this sample; `depthCents` is the nominal peak. */
    float next (float rateHz, float depthCents) noexcept
    {
        phase += rateHz / sr;

        if (phase >= 1.0)
            phase -= 1.0;

        const float shape = shapeAt ((float) phase) * norm;
        const double n = noise.white();
        s1 += (n - s1) * aSlow;  s2 += (s1 - s2) * aSlow;
        j1 += (n - j1) * aJit;   j2 += (j1 - j2) * aJit;

        const float breathe = 1.0f + 0.12f * juce::jlimit (-1.0f, 1.0f, (float) (s2 * normSlow));
        const float jitter = juce::jlimit (-2.0f, 2.0f, (float) (j2 * normJit));

        // Breathing and jitter can push the sum to 1.22; the bounded ceiling rounds the
        // crests off instead (untouched below 0.7, never past 1), so depthCents is a true
        // maximum: the knob's +-30 cents is never exceeded
        const float cents = depthCents * ceiling (shape * breathe + 0.05f * jitter, 1.0f);
        return std::exp2 (cents * (1.0f / 1200.0f)) - 1.0f;
    }

    float getPhase() const noexcept     { return (float) phase; }

private:
    float shapeAt (float p) const noexcept
    {
        return std::sin (p * kTwoPi) + 0.3f * std::sin (2.0f * p * kTwoPi + theta2) + 0.1f * std::sin (3.0f * p * kTwoPi + theta3);
    }

    /** Gain that brings two cascaded one-poles on uniform white noise to unit RMS. */
    static double twoPoleNorm (double a)
    {
        const double r = 1.0 - a;
        const double var = (1.0 / 3.0) * a * a * a * a * (1.0 + r * r) / std::pow (1.0 - r * r, 3.0);
        return 1.0 / std::sqrt (std::max (1.0e-30, var));
    }

    double sr = 48000.0, phase = 0.0;
    float theta2 = 0.0f, theta3 = 0.0f, norm = 1.0f;
    double aSlow = 0.0, aJit = 0.0, normSlow = 1.0, normJit = 1.0, s1 = 0.0, s2 = 0.0, j1 = 0.0, j2 = 0.0;
    lofi::dsp::Noise noise;
};

} // namespace vinyl
