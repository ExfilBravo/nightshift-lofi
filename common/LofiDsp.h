#pragma once

#include <JuceHeader.h>

/** Header-only DSP building blocks shared by the Nightshift Lofi plugins.
    Everything here is deterministic for a given seed, so renders can be compared
    sample for sample, and nothing allocates on the audio thread after prepare().
*/
namespace lofi::dsp
{

inline float dbToGain (float db)    { return juce::Decibels::decibelsToGain (db, -120.0f); }

//==============================================================================
/** xorshift noise: white, or pink via the Paul Kellet filter. */
class Noise
{
public:
    explicit Noise (juce::uint32 seed = 0x9e3779b9u)    { reset (seed); }

    void reset (juce::uint32 seed)
    {
        state = seed != 0 ? seed : 0x9e3779b9u;
        std::fill (std::begin (b), std::end (b), 0.0f);
    }

    /** Uniform white noise in -1..1. */
    float white()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return (float) (state & 0x00ffffffu) * (2.0f / 16777215.0f) - 1.0f;
    }

    /** Pink noise, roughly -1..1. */
    float pink()
    {
        const float w = white();
        b[0] = 0.99886f * b[0] + w * 0.0555179f;
        b[1] = 0.99332f * b[1] + w * 0.0750759f;
        b[2] = 0.96900f * b[2] + w * 0.1538520f;
        b[3] = 0.86650f * b[3] + w * 0.3104856f;
        b[4] = 0.55000f * b[4] + w * 0.5329522f;
        b[5] = -0.7616f * b[5] - w * 0.0168980f;
        const float out = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f;
        b[6] = w * 0.115926f;
        return out * 0.11f;
    }

    /** Uniform in 0..1. */
    float uniform()     { return white() * 0.5f + 0.5f; }

private:
    juce::uint32 state = 0x9e3779b9u;
    float b[7] {};
};

//==============================================================================
/** A slow, wandering modulation source: filtered noise plus an optional sine,
    for wow, flutter, warp and chorus with an organic drift. Output is -1..1. */
class WanderLfo
{
public:
    void prepare (double sampleRate, juce::uint32 seed)
    {
        sr = sampleRate;
        noise.reset (seed);
        phase = 0.0;
        smoothed = 0.0f;
        setRate (1.0f);
    }

    /** Centre rate in Hz; the random part wanders around it. */
    void setRate (float hz)
    {
        rateHz = std::max (0.01f, hz);
        // One-pole on the noise so it moves at roughly the LFO rate
        coeff = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * rateHz / sr));
    }

    /** 0 = pure sine, 1 = pure random wander. */
    void setRandomness (float r)    { randomness = juce::jlimit (0.0f, 1.0f, r); }

    float next()
    {
        phase += rateHz / sr;

        if (phase >= 1.0)
            phase -= 1.0;

        const float sine = (float) std::sin (phase * juce::MathConstants<double>::twoPi);
        smoothed += (noise.white() - smoothed) * coeff;
        const float wander = juce::jlimit (-1.0f, 1.0f, smoothed * 3.0f);
        return sine * (1.0f - randomness) + wander * randomness;
    }

private:
    double sr = 48000.0, phase = 0.0;
    float rateHz = 1.0f, coeff = 0.01f, smoothed = 0.0f, randomness = 0.5f;
    Noise noise;
};

//==============================================================================
/** A fractional delay line with cubic interpolation, for pitch wobble and chorus. */
class ModDelay
{
public:
    void prepare (double sampleRate, float maxDelayMs)
    {
        sr = sampleRate;
        const int length = juce::nextPowerOfTwo ((int) std::ceil (maxDelayMs * 0.001 * sampleRate) + 8);
        buffer.assign ((size_t) length, 0.0f);
        mask = length - 1;
        writePos = 0;
    }

    void reset()
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writePos = 0;
    }

    void push (float x)
    {
        buffer[(size_t) writePos] = x;
        writePos = (writePos + 1) & mask;
    }

    /** The input from `delaySamples` ago, where the sample just pushed is 0 ago: a
        constant read (d) delays by exactly d samples. Fractional values interpolate;
        the range is 1 .. (buffer length - 3), clamped. */
    float read (float delaySamples) const
    {
        // At exactly 1 the cubic needs no future sample (fraction 0), and above 1 the
        // newest sample it touches is the one just pushed
        delaySamples = juce::jlimit (1.0f, (float) (mask - 3), delaySamples);
        const float pos = (float) writePos - 1.0f - delaySamples;
        const int i1 = ((int) std::floor (pos)) & mask;
        const float frac = pos - std::floor (pos);
        const int i0 = (i1 - 1) & mask, i2 = (i1 + 1) & mask, i3 = (i1 + 2) & mask;
        const float y0 = buffer[(size_t) i0], y1 = buffer[(size_t) i1], y2 = buffer[(size_t) i2], y3 = buffer[(size_t) i3];

        // 4-point, 3rd-order Hermite
        const float c0 = y1;
        const float c1 = 0.5f * (y2 - y0);
        const float c2 = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
        const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
        return ((c3 * frac + c2) * frac + c1) * frac + c0;
    }

    float msToSamples (float ms) const    { return (float) (ms * 0.001 * sr); }

private:
    std::vector<float> buffer;
    int mask = 0, writePos = 0;
    double sr = 48000.0;
};

//==============================================================================
/** One-pole lowpass / highpass, cheap enough to run per sample with changing cutoff. */
class OnePole
{
public:
    void prepare (double sampleRate)    { sr = sampleRate; reset(); }
    void reset()                        { z = 0.0f; }

    void setCutoff (float hz)
    {
        a = (float) (1.0 - std::exp (-2.0 * juce::MathConstants<double>::pi * juce::jlimit (1.0f, (float) sr * 0.49f, hz) / sr));
    }

    float lowpass (float x)     { z += (x - z) * a; return z; }
    float highpass (float x)    { return x - lowpass (x); }

private:
    double sr = 48000.0;
    float a = 0.1f, z = 0.0f;
};

/** Removes DC after asymmetric saturation. */
class DcBlocker
{
public:
    void prepare (double sampleRate)    { r = (float) (1.0 - 2.0 * juce::MathConstants<double>::pi * 10.0 / sampleRate); reset(); }
    void reset()                        { x1 = y1 = 0.0f; }

    float process (float x)
    {
        const float y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        return y;
    }

private:
    float r = 0.999f, x1 = 0.0f, y1 = 0.0f;
};

//==============================================================================
/** Waveshapers. All are bounded and continuous; run them oversampled for clean highs. */
namespace shape
{
    /** Soft tube-like curve: gentle until it folds over near 1. */
    inline float soft (float x)
    {
        return x / (1.0f + std::abs (x));
    }

    /** tanh with a cheap rational approximation good to ~1e-4 over -4..4. */
    inline float tanhFast (float x)
    {
        x = juce::jlimit (-4.0f, 4.0f, x);
        const float x2 = x * x;
        return x * (27.0f + x2) / (27.0f + 9.0f * x2);
    }

    /** Tape-style: tanh with a touch of even harmonics from `bias` (0..0.3). */
    inline float tape (float x, float bias)
    {
        return tanhFast (x + bias) - tanhFast (bias);
    }
}

//==============================================================================
/** A parameter that ramps smoothly between values, read once per block or per sample. */
using Smoothed = juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear>;

inline void prepareSmoothed (Smoothed& s, double sampleRate, float initial, double rampSeconds = 0.02)
{
    s.reset (sampleRate, rampSeconds);
    s.setCurrentAndTargetValue (initial);
}

} // namespace lofi::dsp
