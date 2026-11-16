#pragma once

#include <JuceHeader.h>
#include "common/LofiDsp.h"

/** DSP blocks for Nightshift Haze: the chorus LFO bank and the spring. Everything is
    deterministic for a given seed and allocates only in prepare(). */
namespace haze
{

using lofi::dsp::Smoothed;
using lofi::dsp::prepareSmoothed;

//==============================================================================
/** A slow random source: picks a new target every `hold` seconds and glides to it, so
    modulation drifts like a warm motor instead of ticking. Output roughly -1..1. */
class Drift
{
public:
    void prepare (double sampleRate, juce::uint32 seed)
    {
        sr = sampleRate;
        noise.reset (seed);
        value = target = 0.0f;
        counter = 0;
        setHold (1.0f);
    }

    void setHold (float seconds)
    {
        holdSamples = std::max (16, (int) (seconds * sr));
        glide = (float) (1.0 - std::exp (-1.0 / (holdSamples * 0.35)));

        // A shorter hold takes effect now: a countdown started under a long hold (14 s at
        // Rate 0.1 Hz) would otherwise freeze the wander until it ran out. While the hold
        // is unchanged the counter never exceeds it, so this is a no-op then
        counter = std::min (counter, holdSamples);
    }

    float next()
    {
        if (--counter <= 0)
        {
            counter = holdSamples;
            target = noise.white();
        }

        value += (target - value) * glide;
        return value;
    }

private:
    double sr = 48000.0;
    float value = 0.0f, target = 0.0f, glide = 0.001f;
    int holdSamples = 48000, counter = 0;
    lofi::dsp::Noise noise;
};

//==============================================================================
/** The chorus LFO bank: one sine per voice with phase offsets that spread the voices
    around the cycle, each with a touch of drift. Shared by both channels so that
    width 0 is exactly the same chorus on L and R. */
class ChorusLfo
{
public:
    static constexpr int maxVoices = 3;

    void prepare (double sampleRate, juce::uint32 seed)
    {
        sr = sampleRate;
        phase = 0.0;

        for (int v = 0; v < maxVoices; ++v)
        {
            drift[v].prepare (sampleRate, seed + (juce::uint32) (v + 1) * 0x9e3779b9u);
            prepareSmoothed (offset[v], sampleRate, offsetFor (v, maxVoices), 0.12);
        }

        setRate (1.0f);
    }

    /** Lands the voice layout on its target, for a reset or the first block. */
    void snap()
    {
        for (auto& o : offset)
            o.setCurrentAndTargetValue (o.getTargetValue());
    }

    void setRate (float hz)
    {
        rateHz = std::max (0.01f, hz);

        for (auto& d : drift)
            d.setHold (1.4f / rateHz);
    }

    /** Two voices sit opposite each other; three sit 120 degrees apart. The move between
        the two layouts is smoothed, so changing the voice count only glides. */
    void setVoiceCount (int voices)
    {
        for (int v = 0; v < maxVoices; ++v)
            offset[v].setTargetValue (offsetFor (v, voices));
    }

    /** Advances one sample and writes each voice's modulation, -1..1. */
    void next (float* mod)
    {
        phase += rateHz / sr;

        if (phase >= 1.0)
            phase -= 1.0;

        for (int v = 0; v < maxVoices; ++v)
        {
            const float s = std::sin ((float) (phase + offset[v].getNextValue()) * juce::MathConstants<float>::twoPi);
            mod[v] = 0.86f * s + 0.14f * drift[v].next();
        }

        position = mod[0];
    }

    float getPosition() const       { return position; }

private:
    static float offsetFor (int voice, int voices)
    {
        if (voice == 1)
            return voices >= 3 ? 1.0f / 3.0f : 0.5f;

        return voice == 2 ? 2.0f / 3.0f : 0.0f;
    }

    double sr = 48000.0, phase = 0.0;
    float rateHz = 1.0f, position = 0.0f;
    Drift drift[maxVoices];
    Smoothed offset[maxVoices];
};

//==============================================================================
/** RBJ second-order highpass (Butterworth by default), transposed direct form II, fixed
    cutoff. Keeps the bass out of the chorus voices: a delayed copy of 60 Hz fights the
    dry low end, and a one-pole split alone leaves it only 7 dB down. */
class Highpass2
{
public:
    void prepare (double sampleRate, float hz, float q = 0.7071f)
    {
        const double w = juce::MathConstants<double>::twoPi * hz / sampleRate;
        const double alpha = std::sin (w) / (2.0 * q), c = std::cos (w), a0 = 1.0 + alpha;
        b0 = (float) ((1.0 + c) * 0.5 / a0);
        b1 = (float) (-(1.0 + c) / a0);
        a1 = (float) (-2.0 * c / a0);
        a2 = (float) ((1.0 - alpha) / a0);
        reset();
    }

    void reset()    { z1 = z2 = 0.0f; }

    float process (float x)
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b0 * x - a2 * y;
        return y;
    }

private:
    float b0 = 1.0f, b1 = 0.0f, a1 = 0.0f, a2 = 0.0f, z1 = 0.0f, z2 = 0.0f;
};

//==============================================================================
/** A Schroeder allpass with a fixed length, for a little density inside the spring loop. */
class Diffuser
{
public:
    void prepare (double sampleRate, float ms, float g)
    {
        length = std::max (1, (int) (ms * 0.001 * sampleRate));
        const int size = juce::nextPowerOfTwo (length + 4);
        buffer.assign ((size_t) size, 0.0f);
        mask = size - 1;
        pos = 0;
        gain = g;
    }

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        pos = 0;
    }

    float process (float x)
    {
        const float d = buffer[(size_t) ((pos - length) & mask)];
        const float v = x + gain * d;
        buffer[(size_t) pos] = v;
        pos = (pos + 1) & mask;
        return d - gain * v;
    }

    float getLengthSamples() const      { return (float) length; }

private:
    std::vector<float> buffer;
    int length = 1, mask = 0, pos = 0;
    float gain = 0.4f;
};

//==============================================================================
/** A runaway guard for the spring loop, not a saturator. Every element in the loop has
    a gain of 1 or less and the feedback stays below 0.9, so music never reaches the knee:
    a 0 dBFS sine parked on a comb peak builds to about 3 inside the loop, and the knee
    sits at 8 (+18 dBFS). It only matters if a coefficient glide ever pushed the loop
    past unity, and there it rounds off softly, bounded at 12, instead of hard clipping. */
inline float loopLimit (float v)
{
    constexpr float knee = 8.0f, room = 4.0f;
    const float a = std::abs (v);
    return a <= knee ? v : std::copysign (knee + room * lofi::dsp::shape::tanhFast ((a - knee) / room), v);
}

//==============================================================================
/** One spring: the wave runs down the coil (a delay, the transit time), is picked up at
    the far transducer, reflects and runs back (a second transit) to start over, so the
    first chirp arrives after one transit and the echoes follow every round trip, as on
    a real tank. On its way it meets a damping lowpass (tone), a low cut, and a chain of
    dispersive second-order allpass sections whose poles sit between 700 Hz and 2 kHz.
    Below that band the group delay rises with frequency, so each echo becomes a chirp
    that starts low and piles up around 1 kHz: the boing. `drip` fades the sections in
    one after another at full radius, so the chirp grows evenly with the knob; a section
    at radius 0 is a plain two-sample delay. */
class Spring
{
public:
    static constexpr int sections = 40;

    struct Shape
    {
        float lengthScale;        // loop length relative to the nominal spring
        float poleLow, poleHigh;  // dispersion band, Hz
        float diffuserMs;         // short allpass inside the loop
    };

    void prepare (double sampleRate, juce::uint32 seed, Shape s)
    {
        sr = sampleRate;
        shape = s;
        maxRadius = std::pow (0.96f, (float) (48000.0 / sampleRate));   // keeps the chirp the same length in ms at any rate

        outbound.prepare (sampleRate, 70.0f);    // half of the longest loop (92 ms) plus the drift
        inbound.prepare (sampleRate, 70.0f);
        diffuser.prepare (sampleRate, s.diffuserMs, 0.38f);

        for (int i = 0; i < sections; ++i)
        {
            const float f = s.poleLow * std::pow (s.poleHigh / s.poleLow, (float) i / (float) (sections - 1));
            poleCos[i] = std::cos (juce::MathConstants<float>::twoPi * f / (float) sr);
        }

        reset();
        drift.prepare (sampleRate, seed);
        drift.setHold (1.3f);
        driftSamples = (float) (driftMs * 0.001 * sampleRate);   // 2.5 samples at 48 kHz, the same time at any rate
        lowCutA = coefficientFor (70.0f);

        // The tank's geometry glides slowly: a size or drip jump retunes 30 ms of delay and
        // dispersion, which over the usual 50 ms would swoop the tail by an octave
        prepareSmoothed (loopSamples, sr, loopLengthFor (0.5f), 0.25);
        prepareSmoothed (feedback, sr, 0.8f, 0.25);
        prepareSmoothed (drip, sr, 0.0f, 0.25);
        prepareSmoothed (dampA, sr, coefficientFor (3000.0f), 0.03);
        setTargets (0.5f, 0.5f, 0.4f);
        loopSamples.setCurrentAndTargetValue (loopSamples.getTargetValue());
        feedback.setCurrentAndTargetValue (feedback.getTargetValue());
        drip.setCurrentAndTargetValue (drip.getTargetValue());
        dampA.setCurrentAndTargetValue (dampA.getTargetValue());
    }

    /** Silences the spring: delay lines, allpass and filter states. No allocation. */
    void reset()
    {
        outbound.reset();
        inbound.reset();
        diffuser.reset();
        std::fill (std::begin (s1), std::end (s1), 0.0f);
        std::fill (std::begin (s2), std::end (s2), 0.0f);
        dampZ = lowCutZ = 0.0f;
    }

    /** Lands every ramp on its target, for a reset or the first block. */
    void snap()
    {
        for (auto* s : { &loopSamples, &feedback, &drip, &dampA })
            s->setCurrentAndTargetValue (s->getTargetValue());
    }

    /** size, tone and drip in 0..1; call once per block. */
    void setTargets (float size, float tone, float dripAmount)
    {
        const float length = loopLengthFor (size);
        dripAmount = juce::jlimit (0.0f, 1.0f, dripAmount);

        // Low-frequency group delay of the allpass chain, so the feedback gives the asked T60
        float chainDelay = 0.0f;

        for (int i = 0; i < sections; ++i)
        {
            const float r = sectionRadius (dripAmount * (float) (sections + 1), i);
            chainDelay += 2.0f * (1.0f - r * r) / (1.0f - 2.0f * r * poleCos[i] + r * r);
        }

        // 0.6 s, 2 s, 4 s at 0, 50, 100. The factor makes up for the Schroeder curve's
        // steeper start while the damped highs are still dying; measured, not derived
        const float t60 = (1.05f + 0.17f * size) * nominalT60 (size);
        const float loopSeconds = (float) ((length + chainDelay + diffuser.getLengthSamples() * 0.5f) / sr);
        const float g = std::pow (10.0f, -3.0f * loopSeconds / t60);

        loopSamples.setTargetValue (length);
        drip.setTargetValue (dripAmount);
        feedback.setTargetValue (g);
        dampA.setTargetValue (coefficientFor (900.0f * std::pow (10.0f, tone)));    // 900 Hz .. 9 kHz
        currentFeedback = g;
    }

    float getFeedback() const       { return currentFeedback; }

    /** The decay the tank is tuned for at `size` 0..1, in seconds: 0.6 / 2 / 4 s at
        0 / 50 / 100 %. The editor's DECAY readout shows this same number. */
    static float nominalT60 (float size)
    {
        size = juce::jlimit (0.0f, 1.0f, size);
        return 0.6f + 2.2f * size + 1.2f * size * size;
    }

    float process (float x)
    {
        // The loop is split in two equal transits; the drift (a coil that is never quite
        // still) moves only the outbound one, by about 50 microseconds
        const float transit = loopSamples.getNextValue() * 0.5f;
        outbound.push (x + loopLimit (inbound.read (transit) * feedback.getNextValue()));
        float y = outbound.read (transit + driftSamples * drift.next());

        // Tone damping and a low cut, so the tail darkens and never builds up mud
        dampZ += (y - dampZ) * dampA.getNextValue();
        y = dampZ;
        lowCutZ += (y - lowCutZ) * lowCutA;
        y -= lowCutZ;

        // Dispersion: the chirp
        const float active = drip.getNextValue() * (float) (sections + 1);

        for (int i = 0; i < sections; ++i)
        {
            const float r = sectionRadius (active, i);
            const float a1 = -2.0f * r * poleCos[i];
            const float a2 = r * r;
            const float out = a2 * y + s1[i];
            s1[i] = a1 * y + s2[i] - a1 * out;
            s2[i] = y - a2 * out;
            y = out;
        }

        y = diffuser.process (y);
        inbound.push (y);
        return y;
    }

private:
    float loopLengthFor (float size) const
    {
        return (float) ((30.0 + 40.0 * size) * shape.lengthScale * 0.001 * sr);
    }

    /** Section i is fully on once `active` (drip scaled to the section count) passes i + 1. */
    float sectionRadius (float active, int i) const
    {
        return maxRadius * juce::jlimit (0.0f, 1.0f, active - (float) i);
    }

    float coefficientFor (float hz) const
    {
        return (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * juce::jlimit (10.0f, (float) sr * 0.45f, hz) / sr));
    }

    static constexpr double driftMs = 2.5 / 48.0;

    double sr = 48000.0;
    Shape shape { 1.0f, 700.0f, 2000.0f, 4.1f };
    float maxRadius = 0.96f, currentFeedback = 0.8f, driftSamples = 2.5f;
    lofi::dsp::ModDelay outbound, inbound;
    Diffuser diffuser;
    Drift drift;
    float poleCos[sections] {}, s1[sections] {}, s2[sections] {};
    float dampZ = 0.0f, lowCutZ = 0.0f, lowCutA = 0.01f;
    Smoothed loopSamples, feedback, drip, dampA;
};

} // namespace haze
