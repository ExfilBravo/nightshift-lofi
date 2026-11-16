#pragma once

#include <JuceHeader.h>

/** Measurements for judging renders without ears. All take the first channel. */
namespace measure
{

struct Stats
{
    double peakDb = -200.0, rmsDb = -200.0, dc = 0.0;
    int clipped = 0;
    bool finite = true;
};

Stats stats (const juce::AudioBuffer<float>&);

/** Follows a (near) sine's frequency over time, for wow, flutter, warp and chorus. */
struct PitchTrack
{
    double meanHz = 0.0;
    double minCents = 0.0, maxCents = 0.0, rmsCents = 0.0;   // deviation from the mean
    double modRateHz = 0.0;                                   // strongest modulation rate
    int windows = 0;
};

PitchTrack pitchTrack (const juce::AudioBuffer<float>&, double sampleRate, double windowMs = 20.0);

/** Harmonic distortion of a sine at f0, from a Hann-windowed FFT of the steady middle. */
struct Thd
{
    double fundamentalHz = 0.0, fundamentalDb = -200.0;
    double thdPercent = 0.0, thdDb = -200.0, thdnPercent = 0.0;
    double evenPercent = 0.0, oddPercent = 0.0;               // 2nd+4th... vs 3rd+5th...
    double noiseFloorDb = -200.0;
};

Thd thd (const juce::AudioBuffer<float>&, double sampleRate, double f0);

/** Decay of an impulse response: T60 by Schroeder integration, plus how the spectrum
    darkens over time. */
struct Decay
{
    double t60 = 0.0, t20 = 0.0;
    double earlyCentroidHz = 0.0, lateCentroidHz = 0.0;
    double peakDb = -200.0;
};

Decay decay (const juce::AudioBuffer<float>&, double sampleRate);

/** Energy share per band of the whole file, in percent, and the spectral centroid. */
struct Bands
{
    double sub = 0.0, bass = 0.0, lowMid = 0.0, mid = 0.0, high = 0.0, air = 0.0;
    double centroidHz = 0.0;
};

Bands bands (const juce::AudioBuffer<float>&, double sampleRate);

/** Average level in dB per 1/3-octave-ish band centre, for frequency-response checks. */
std::vector<std::pair<double, double>> responseAt (const juce::AudioBuffer<float>&, double sampleRate, const std::vector<double>& freqs);

juce::String describe (const Stats&);
juce::String describe (const PitchTrack&);
juce::String describe (const Thd&);
juce::String describe (const Decay&);
juce::String describe (const Bands&);

} // namespace measure
