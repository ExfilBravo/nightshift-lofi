#pragma once

#include <JuceHeader.h>
#include "common/LofiDsp.h"

/** DSP blocks for Nightshift Crush. Everything is header-only, allocation-free after
    prepare, and deterministic (there is no noise source in a converter emulation: the
    grit is the quantisation error itself).

    The converter keeps one property above all: the wet path is a constant, integer
    delay at every `rate` and `alias` setting. Every filter in it is linear phase and
    every hold is centred on the instant it represents, so the latency the plugin
    reports is what an impulse measures, and Mix below 100 % blends without combing.
*/
namespace crush
{

//==============================================================================
/** The resonant output filter: a 2-pole lowpass whose magnitude matches the analog
    prototype all the way to Nyquist (M. Vicanek, "Matched Second Order Digital Filters",
    2016: impulse-invariant poles, numerator fitted at DC, cutoff and Nyquist). A bilinear
    filter set to 12 kHz in a 48 kHz session is 20 dB darker at 21 kHz than the analog
    filter it stands for; this one stays within about 1 dB. Coefficients are shared by
    the channels; the states are per channel. */
struct MatchedLowpassCoefficients
{
    void set (double fc, double q, double sampleRate)
    {
        const double w0 = juce::MathConstants<double>::twoPi * juce::jlimit (10.0, 0.49 * sampleRate, fc) / sampleRate;
        const double zeta = 0.5 / q;
        const double e = std::exp (-zeta * w0);
        const double c1 = zeta <= 1.0 ? -2.0 * e * std::cos (std::sqrt (1.0 - zeta * zeta) * w0)
                                      : -2.0 * e * std::cosh (std::sqrt (zeta * zeta - 1.0) * w0);
        const double c2 = e * e;
        const double A0 = (1.0 + c1 + c2) * (1.0 + c1 + c2);
        const double A1 = (1.0 - c1 + c2) * (1.0 - c1 + c2);
        const double A2 = -4.0 * c2;
        const double s = std::sin (0.5 * w0);
        const double phi1 = s * s, phi0 = 1.0 - phi1, phi2 = 4.0 * phi0 * phi1;
        const double R1 = (A0 * phi0 + A1 * phi1 + A2 * phi2) * q * q;
        const double B0 = A0, B1 = std::max (0.0, (R1 - B0 * phi0) / phi1);

        b0 = (float) (0.5 * (std::sqrt (B0) + std::sqrt (B1)));
        b1 = (float) (std::sqrt (B0) - b0);
        a1 = (float) c1;
        a2 = (float) c2;
    }

    float b0 = 1.0f, b1 = 0.0f, a1 = 0.0f, a2 = 0.0f;
};

class MatchedLowpass
{
public:
    void reset()    { s1 = s2 = 0.0f; }

    float process (float x, const MatchedLowpassCoefficients& c)
    {
        const float y = c.b0 * x + s1;
        s1 = c.b1 * x - c.a1 * y + s2;
        s2 = -c.a2 * y;
        return y;
    }

private:
    float s1 = 0.0f, s2 = 0.0f;
};

//==============================================================================
inline double besselI0 (double x)
{
    double term = 1.0, total = 1.0;
    const double q = x * x * 0.25;

    for (int k = 1; k < 80 && term > 1e-17 * total; ++k)
    {
        term *= q / ((double) k * k);
        total += term;
    }

    return total;
}

/** Half-length of the plain interpolator, in host samples: about 0.67 ms. */
inline int plainHalfLength (double sampleRate)
{
    return juce::jlimit (8, 256, (int) std::round (sampleRate / 1500.0));
}

//==============================================================================
/** The plain interpolator that reads the input between host samples when nothing is
    anti-aliased: a Kaiser-windowed sinc at 0.47 of the host rate, 2 * half taps,
    tabulated at `phases` + 1 fractional positions, each row normalised to unity gain.
    Tap j of the row for position f sits at time (1 - half + j) - f from the read point. */
struct PlainInterpolator
{
    static constexpr int phases = 256;
    static constexpr double cutoff = 0.47;      // cycles per host sample
    static constexpr double beta = 6.0;         // Kaiser: about 63 dB of stopband

    void prepare (int halfLength)
    {
        half = halfLength;
        taps = 2 * half;
        table.assign ((size_t) ((phases + 1) * taps), 0.0f);
        const double windowNorm = besselI0 (beta);

        for (int row = 0; row <= phases; ++row)
        {
            const double f = (double) row / phases;
            double sum = 0.0;
            float* r = table.data() + row * taps;

            for (int j = 0; j < taps; ++j)
            {
                const double tau = (double) (1 - half + j) - f;
                const double sinc = std::abs (tau) < 1.0e-9 ? 2.0 * cutoff
                                                            : std::sin (juce::MathConstants<double>::twoPi * cutoff * tau)
                                                                / (juce::MathConstants<double>::pi * tau);
                const double u = tau / half;
                const double w = std::abs (u) >= 1.0 ? 0.0 : besselI0 (beta * std::sqrt (1.0 - u * u)) / windowNorm;
                r[j] = (float) (sinc * w);
                sum += sinc * w;
            }

            // Normalise, and store the row oldest-first so it runs along the history
            std::vector<float> normalised ((size_t) taps);

            for (int j = 0; j < taps; ++j)
                normalised[(size_t) j] = (float) (r[j] / sum);

            for (int j = 0; j < taps; ++j)
                r[j] = normalised[(size_t) (taps - 1 - j)];
        }
    }

    /** The kernel at position f (0..1) against `window`, which holds the taps' samples
        contiguously, oldest first: window[taps - 1] is the newest, the sample at the
        smallest delay the kernel touches. */
    float apply (double f, const float* window) const
    {
        const float pf = (float) f * (float) phases;
        const int p = std::min (phases - 1, (int) pf);
        const float fr = pf - (float) p;
        const float* a = table.data() + p * taps;
        const float* b = a + taps;
        float acc = 0.0f;

        for (int j = 0; j < taps; ++j)
            acc += (a[j] + (b[j] - a[j]) * fr) * window[j];

        return acc;
    }

    std::vector<float> table;
    int half = 32, taps = 64;
};

//==============================================================================
/** The anti-alias and reconstruction lowpass, defined in converter periods so its shape
    follows the rate: a Kaiser-windowed sinc reaching K periods to each side, 60 dB of
    stopband from the converter's Nyquist up, the cutoff as high as K allows (about
    0.46 x rate at K = 24, 0.40 x rate at K = 9). Tabulated for whole K from kMin to kMax
    at 256 points per period; a fractional K blends its two neighbours, so the filter
    glides with the rate. Each row integrates to 1. Built once, on first use. */
struct ConverterKernel
{
    static constexpr int kMin = 6, kMax = 24, perPeriod = 256;
    static constexpr int stride = kMax * perPeriod + 2;
    static constexpr double stopbandDb = 60.0;

    /** Cutoff in converter-rate units (0.5 = the converter's Nyquist) for K periods. */
    static double cutoffFor (int periods)
    {
        const double transition = (stopbandDb - 7.95) / (2.285 * 2.0 * juce::MathConstants<double>::twoPi * periods);
        return 0.5 - 0.5 * transition - 0.004;
    }

    static const std::vector<float>& table()
    {
        static const std::vector<float> t = []
        {
            std::vector<float> out ((size_t) ((kMax - kMin + 1) * stride), 0.0f);
            const double beta = 0.1102 * (stopbandDb - 8.7);
            const double windowNorm = besselI0 (beta);
            std::vector<double> row ((size_t) stride);

            for (int K = kMin; K <= kMax; ++K)
            {
                const double fc = cutoffFor (K);

                for (int i = 0; i < stride; ++i)
                {
                    const double tau = (double) i / perPeriod;

                    if (tau >= K)
                    {
                        row[(size_t) i] = 0.0;
                        continue;
                    }

                    const double sinc = i == 0 ? 2.0 * fc : std::sin (juce::MathConstants<double>::twoPi * fc * tau) / (juce::MathConstants<double>::pi * tau);
                    const double u = tau / K;
                    row[(size_t) i] = sinc * besselI0 (beta * std::sqrt (std::max (0.0, 1.0 - u * u))) / windowNorm;
                }

                // The kernel is even: its integral over -K..K is the doubled half
                double integral = row[0];

                for (int i = 1; i < K * perPeriod; ++i)
                    integral += 2.0 * row[(size_t) i];

                integral /= perPeriod;
                float* dest = out.data() + (K - kMin) * stride;

                for (int i = 0; i < stride; ++i)
                    dest[i] = (float) (row[(size_t) i] / integral);
            }

            return out;
        }();

        return t;
    }

    /** The two rows and the blend for a (fractional) K, plus how far it reaches. */
    struct Shape
    {
        const float* a = nullptr;
        const float* b = nullptr;
        float blend = 0.0f;
        int reach = kMax;       // whole periods to each side with any weight
    };

    static Shape shapeFor (double periods)
    {
        const double k = juce::jlimit ((double) kMin, (double) kMax, periods);
        const int whole = std::min (kMax, (int) k);
        const auto& t = table();
        Shape s;
        s.a = t.data() + (whole - kMin) * stride;
        s.b = t.data() + (std::min (kMax, whole + 1) - kMin) * stride;
        s.blend = (float) (k - whole);
        s.reach = s.blend > 0.0f ? whole + 1 : whole;
        return s;
    }

    /** The kernel at |tau| x perPeriod (0 at and beyond the reach). */
    static float atIndex (const Shape& s, float p)
    {
        const int i = (int) p;

        if (i >= stride - 1)
            return 0.0f;

        const float f = p - (float) i;
        const float va = s.a[i] + (s.a[i + 1] - s.a[i]) * f;

        if (s.blend <= 0.0f)
            return va;

        const float vb = s.b[i] + (s.b[i + 1] - s.b[i]) * f;
        return va + (vb - va) * s.blend;
    }

    /** The kernel at |tau| periods. */
    static float at (const Shape& s, float absTau)    { return atIndex (s, absTau * (float) perPeriod); }
};

//==============================================================================
/** Truncation to a word length, with fractional bits as a crossfade between the two
    neighbouring word lengths so the parameter can be smoothed without a click. */
struct Quantiser
{
    void setBits (float bits)
    {
        const float lo = std::floor (bits);
        frac = bits - lo;
        q0 = std::ldexp (1.0f, (int) lo - 1);     // 2^(bits-1) levels per polarity
        q1 = q0 * 2.0f;
    }

    float operator() (float x) const
    {
        const float a = std::floor (x * q0) / q0;

        if (frac <= 0.0f)
            return a;

        const float b = std::floor (x * q1) / q1;
        return a + (b - a) * frac;
    }

    float q0 = 2048.0f, q1 = 4096.0f, frac = 0.0f;
};

//==============================================================================
/** The converter for one channel: sample at any rate with sub-sample timing, truncate,
    then either hold (alias 100) or rebuild the waveform from the samples (alias 0).

    Sampling: at each instant the input is read with the plain interpolator (alias 100:
    everything above rate/2 folds) crossfaded by `flat` against the anti-alias lowpass
    (ConverterKernel, stopband from rate/2). Both are linear phase around the same point.

    Holding: every hold transition is a band-limited step (windowed-sinc BLEP), so the
    output carries the images and sinc droop of a real zero-order hold without the grid
    jitter of a counter-based crusher, and nothing folds back from above the host's
    Nyquist. The value held from an instant is read half a hold period later than that
    instant, which centres the staircase on the signal.

    Rebuilding: the samples are interpolated back to the host rate with the same
    ConverterKernel (images of rate/2 and up removed by 60 dB), evaluated at exactly the
    instant the staircase's centre stands for. The output crossfades the staircase into
    the rebuilt waveform by `flat`; the two share their delay, so nothing combs.

    Both kernels reach K periods to each side, K = min (24, budget x rate / sr - 1), and
    `budget` is 2.5 ms of host samples: the filters keep their 60 dB at every rate down
    to 4 kHz, and only their cutoff slides down a little at the lowest rates.

    Latency: getLatency() samples, the same at every rate and alias setting. */
class Sampler
{
public:
    static constexpr int kHalf = 48, kLen = 2 * kHalf, kOver = 128;
    static constexpr double minRate = 4000.0;
    static constexpr double budgetSeconds = 0.0025;

    /** Band-limited step residuals, one row of kLen taps for each of kOver + 1 sub-sample
        positions of the step. Tap j of row r lands j - kHalf + r / kOver samples after the
        step instant and holds B (that time) - (j >= kHalf ? 1 : 0): the band-limited unit
        step B (the running integral of a Kaiser-windowed sinc, 96 taps, 84 dB stopband,
        cutoff 0.472 fs, so the passband reaches 0.444 fs and the stopband starts at
        Nyquist) minus the naive step the staircase already carries at that output sample.
        The naive step is decided by the tap, not by the time, so interpolating between
        two rows never crosses the step's jump. */
    static const std::vector<float>& stepRows()
    {
        static const std::vector<float> t = []
        {
            constexpr int size = kLen * kOver + 1;
            std::vector<double> smooth ((size_t) size, 0.0);
            const double fc = 0.472, beta = 8.3;
            const double windowNorm = besselI0 (beta);
            double sum = 0.0, prev = 0.0;

            for (int m = 0; m < size; ++m)
            {
                const double time = -kHalf + (double) m / kOver;
                const double x = juce::MathConstants<double>::twoPi * fc * time;
                const double sinc = std::abs (x) < 1e-9 ? 1.0 : std::sin (x) / x;
                const double r = time / kHalf;
                const double w = std::abs (r) >= 1.0 ? 0.0 : besselI0 (beta * std::sqrt (1.0 - r * r)) / windowNorm;
                const double h = 2.0 * fc * sinc * w;
                sum += 0.5 * (prev + h) / kOver;
                prev = h;
                smooth[(size_t) m] = sum;
            }

            std::vector<float> out ((size_t) ((kOver + 1) * kLen));

            for (int r = 0; r <= kOver; ++r)
                for (int j = 0; j < kLen; ++j)
                    out[(size_t) (r * kLen + j)] = (float) (smooth[(size_t) (j * kOver + r)] / sum - (j >= kHalf ? 1.0 : 0.0));

            return out;
        }();

        return t;
    }

    void prepare (double sampleRate)
    {
        sr = sampleRate;
        stepTable = stepRows().data();
        ConverterKernel::table();

        const int plainHalf = plainHalfLength (sampleRate);
        budget = std::max (8, (int) std::round (sampleRate * budgetSeconds));
        const int reach = std::max (budget, plainHalf);
        const double longestHold = sampleRate / minRate;

        // The read point sits half a hold ahead of the base delay, and the kernels need
        // `reach` samples on its newer side; the rebuilt waveform needs `extra` more
        // samples of the converter's output before it can be evaluated
        baseDelay = reach + (int) std::ceil (0.5 * longestHold) + 1;
        extra = std::max (budget, kHalf);
        stepOffset = extra - kHalf;

        // The history is mirrored (each sample written twice, `historySize` apart), so
        // any window of it is contiguous
        historySize = juce::nextPowerOfTwo (baseDelay + reach + (int) longestHold + 8);
        history.assign ((size_t) (2 * historySize), 0.0f);
        stair.assign ((size_t) juce::nextPowerOfTwo (extra + 2), 0.0f);
        stairMask = (int) stair.size() - 1;
        ringSize = juce::nextPowerOfTwo (stepOffset + kLen + 2);
        ring.assign ((size_t) ringSize, 0.0f);
        samples.assign ((size_t) juce::nextPowerOfTwo (extra + 2 * ConverterKernel::kMax + 8), Sample {});
        sampleMask = (int64_t) samples.size() - 1;

        plain.prepare (plainHalf);
        setRate (26040.0);
        reset();
    }

    /** Samples between an input and its image at the output, for any rate and alias. */
    int getLatency() const      { return baseDelay + extra; }

    void reset()
    {
        held = 0.0f;
        phase = 0.0;
        clock = 0.0;
        std::fill (history.begin(), history.end(), 0.0f);
        std::fill (stair.begin(), stair.end(), 0.0f);
        std::fill (ring.begin(), ring.end(), 0.0f);
        std::fill (samples.begin(), samples.end(), Sample {});
        newest = sw = rw = 0;
        written = first = last = 0;
    }

    /** Converter rate in Hz (clamped to minRate .. the host rate). Cheap; call per sample. */
    void setRate (double hz)
    {
        inc = juce::jlimit (minRate / sr, 1.0, hz / sr);
        shape = ConverterKernel::shapeFor (std::min ((double) ConverterKernel::kMax, budget * inc - 1.0));
    }

    /** `flat` 1 = plain sample-and-hold (full aliasing), 0 = anti-aliased sampling and
        a rebuilt waveform; in between, both crossfade. */
    template <typename Quantise>
    float process (float x, float flat, Quantise&& quantise)
    {
        newest = (newest + 1) & (historySize - 1);
        history[(size_t) newest] = x;
        history[(size_t) (newest + historySize)] = x;
        clock += 1.0;
        phase += inc;

        if (phase >= 1.0)
        {
            phase -= 1.0;
            const double d = phase / inc;                  // samples since the sampling instant
            const double at = d + baseDelay - 0.5 / inc;   // delay of the input it stands for
            float v = 0.0f;

            if (flat > 0.0f)
                v += flat * readPlain (at);

            if (flat < 1.0f)
                v += (1.0f - flat) * readFiltered (at);

            const float value = quantise (v);
            auto& s = samples[(size_t) (written & sampleMask)];
            s.value = value;
            s.time = clock - at;
            ++written;

            const float step = value - held;
            held = value;

            if (step != 0.0f)
                addStep (step, d);
        }

        // The staircase, `extra` samples late: the naive hold plus the band-limiting
        // residuals that landed on this sample
        stair[(size_t) sw] = held;
        const float zoh = stair[(size_t) ((sw - extra) & stairMask)] + ring[(size_t) rw];
        sw = (sw + 1) & stairMask;
        ring[(size_t) rw] = 0.0f;
        rw = (rw + 1) & (ringSize - 1);

        track();

        if (flat >= 1.0f)
            return zoh;

        const float rebuilt = rebuild();
        return rebuilt + (zoh - rebuilt) * flat;
    }

private:
    struct Sample
    {
        float value = 0.0f;
        double time = -1.0e12;      // the input instant it stands for, on `clock`
    };

    /** The `count` history samples ending `delay` samples ago, oldest first. */
    const float* window (int delay, int count) const
    {
        const int end = (newest - delay) & (historySize - 1);
        return history.data() + end + historySize - count + 1;
    }

    /** Adds a band-limited step of `step` at `d` samples before the current one. */
    void addStep (float step, double d)
    {
        const float dOver = (float) d * (float) kOver;
        const int di = juce::jlimit (0, kOver - 1, (int) dOver);
        const float f = dOver - (float) di;
        const float* a = stepTable + di * kLen;
        const float* b = a + kLen;

        // Contiguous runs of the ring, split where it wraps
        const int start = (rw + stepOffset) & (ringSize - 1);
        const int firstRun = std::min (kLen, ringSize - start);
        float* dest = ring.data() + start;

        for (int j = 0; j < firstRun; ++j)
            dest[j] += step * (a[j] + (b[j] - a[j]) * f);

        for (int j = firstRun; j < kLen; ++j)
            ring[(size_t) (j - firstRun)] += step * (a[j] + (b[j] - a[j]) * f);
    }

    float readPlain (double at) const
    {
        const int i0 = (int) std::floor (at);
        return plain.apply (at - i0, window (i0 - plain.half + 1, plain.taps));
    }

    /** The anti-alias lowpass at `at` samples ago: the kernel against the host samples
        within its reach, scaled by the sample spacing in periods (rows integrate to 1). */
    float readFiltered (double at) const
    {
        const double span = shape.reach / inc;
        const int lo = std::max (0, (int) std::ceil (at - span));
        const int hi = (int) std::floor (at + span);
        const int count = hi - lo + 1;
        const float* x = window (lo, count);               // x[k] is hi - k samples ago
        const float scale = (float) inc * (float) ConverterKernel::perPeriod;
        const float p0 = (float) ((hi - at) * inc * ConverterKernel::perPeriod);
        float acc = 0.0f;

        for (int k = 0; k < count; ++k)
            acc += ConverterKernel::atIndex (shape, std::abs (p0 - (float) k * scale)) * x[k];

        return acc * (float) inc;
    }

    /** Keeps [first, last) on the samples whose kernel can touch the instant to rebuild. */
    void track()
    {
        const double when = clock - getLatency();
        const double limit = ConverterKernel::kMax + 2;

        first = std::max (first, written - (int64_t) samples.size() + 1);

        while (first < written && (when - samples[(size_t) (first & sampleMask)].time) * inc >= limit)
            ++first;

        last = std::max (last, first);

        while (last < written && (when - samples[(size_t) (last & sampleMask)].time) * inc > -limit)
            ++last;

        // Keep the clock small so the sample times stay precise
        if (clock >= 1073741824.0)
        {
            clock -= 1073741824.0;

            for (auto& s : samples)
                s.time -= 1073741824.0;
        }
    }

    /** The samples interpolated back at the instant the output stands for. */
    float rebuild() const
    {
        const double when = clock - getLatency();
        const double scale = inc * ConverterKernel::perPeriod;
        const float end = (float) (shape.reach * ConverterKernel::perPeriod);
        float acc = 0.0f;

        for (int64_t i = first; i < last; ++i)
        {
            const auto& s = samples[(size_t) (i & sampleMask)];
            const float p = (float) std::abs ((when - s.time) * scale);

            if (p < end)
                acc += s.value * ConverterKernel::atIndex (shape, p);
        }

        return acc;
    }

    double sr = 48000.0, phase = 0.0, inc = 0.5, clock = 0.0;
    int budget = 120, baseDelay = 127, extra = 120, stepOffset = 72;
    ConverterKernel::Shape shape;
    PlainInterpolator plain;
    const float* stepTable = nullptr;

    std::vector<float> history, stair, ring;
    int historySize = 512, stairMask = 127, ringSize = 256;
    int newest = 0, sw = 0, rw = 0;
    float held = 0.0f;

    std::vector<Sample> samples;
    int64_t sampleMask = 255, written = 0, first = 0, last = 0;
};

//==============================================================================
/** The converter's input stage: linear to `knee`, then a smooth fold into the rails at
    1.0, continuous in value and slope. Run oversampled. */
inline float inputClip (float x)
{
    constexpr float knee = 0.75f, span = 1.0f - knee;
    const float a = std::abs (x);

    if (a <= knee)
        return x;

    const float y = knee + span * lofi::dsp::shape::tanhFast ((a - knee) / span);
    return x < 0.0f ? -y : y;
}

} // namespace crush
