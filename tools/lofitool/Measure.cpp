#include "Measure.h"

namespace measure
{

namespace
{
    double db (double g)    { return g > 0.0 ? 20.0 * std::log10 (g) : -200.0; }

    // 4-term Blackman-Harris: sidelobes 92 dB down, so a clean sine leaks nothing
    // measurable into the "noise" when THD+N is summed over the whole spectrum
    constexpr double windowPowerGain = 0.257963;   // sum of w^2 / N

    /** Blackman-Harris-windowed power spectrum of `n` samples starting at `start`; bins 0..n/2. */
    std::vector<double> powerSpectrum (const float* x, int start, int order)
    {
        const int n = 1 << order;
        juce::dsp::FFT fft (order);
        std::vector<float> data ((size_t) n * 2, 0.0f);

        for (int i = 0; i < n; ++i)
        {
            const float t = juce::MathConstants<float>::twoPi * i / (n - 1);
            const float w = 0.35875f - 0.48829f * std::cos (t) + 0.14128f * std::cos (2.0f * t) - 0.01168f * std::cos (3.0f * t);
            data[(size_t) i] = x[start + i] * w;
        }

        fft.performFrequencyOnlyForwardTransform (data.data(), true);
        std::vector<double> power ((size_t) n / 2 + 1);

        for (size_t k = 0; k < power.size(); ++k)
            power[k] = (double) data[k] * data[k];

        return power;
    }
}

//==============================================================================
Stats stats (const juce::AudioBuffer<float>& b)
{
    Stats s;
    double sumSq = 0.0, sum = 0.0;
    float peak = 0.0f;
    const int n = b.getNumSamples();

    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        auto* d = b.getReadPointer (ch);

        for (int i = 0; i < n; ++i)
        {
            if (! std::isfinite (d[i]))
            {
                s.finite = false;
                continue;
            }

            peak = std::max (peak, std::abs (d[i]));
            sumSq += (double) d[i] * d[i];
            sum += d[i];

            if (std::abs (d[i]) >= 0.999f)
                ++s.clipped;
        }
    }

    const double count = (double) n * std::max (1, b.getNumChannels());
    s.peakDb = db (peak);
    s.rmsDb = db (std::sqrt (sumSq / std::max (1.0, count)));
    s.dc = sum / std::max (1.0, count);
    return s;
}

//==============================================================================
PitchTrack pitchTrack (const juce::AudioBuffer<float>& b, double sr, double windowMs)
{
    PitchTrack t;
    const int n = b.getNumSamples();
    auto* x = b.getReadPointer (0);
    const int window = std::max (64, (int) (sr * windowMs / 1000.0));

    // Frequency per window from interpolated positive zero crossings
    std::vector<double> freqs;

    for (int start = 0; start + window <= n; start += window)
    {
        double first = -1.0, last = -1.0;
        int crossings = 0;

        for (int i = start + 1; i < start + window; ++i)
        {
            if (x[i - 1] <= 0.0f && x[i] > 0.0f)
            {
                const double frac = x[i - 1] / (double) (x[i - 1] - x[i]);   // where it crossed between i-1 and i
                const double pos = i - 1 + frac;

                if (first < 0.0)
                    first = pos;
                else
                    ++crossings;

                last = pos;
            }
        }

        if (crossings >= 2)
            freqs.push_back (sr * crossings / (last - first));
    }

    t.windows = (int) freqs.size();

    if (freqs.size() < 4)
        return t;

    // Drop the first and last windows, which can hold edge effects
    freqs.erase (freqs.begin());
    freqs.pop_back();

    double sum = 0.0;

    for (auto f : freqs)
        sum += f;

    t.meanHz = sum / (double) freqs.size();
    double sumSq = 0.0;
    std::vector<double> cents;

    for (auto f : freqs)
    {
        const double c = 1200.0 * std::log2 (f / t.meanHz);
        cents.push_back (c);
        t.minCents = std::min (t.minCents, c);
        t.maxCents = std::max (t.maxCents, c);
        sumSq += c * c;
    }

    t.rmsCents = std::sqrt (sumSq / (double) cents.size());

    // Modulation rate: strongest bin of the deviation signal, sampled at 1/window
    const double devRate = sr / window;
    int order = 1;

    while ((1 << order) < (int) cents.size())
        ++order;

    const int fftSize = 1 << order;
    juce::dsp::FFT fft (order);
    std::vector<float> data ((size_t) fftSize * 2, 0.0f);

    for (size_t i = 0; i < cents.size(); ++i)
        data[i] = (float) cents[i] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) (cents.size() - 1)));

    fft.performFrequencyOnlyForwardTransform (data.data(), true);
    int best = 1;

    for (int k = 2; k < fftSize / 2; ++k)
        if (data[(size_t) k] > data[(size_t) best])
            best = k;

    t.modRateHz = best * devRate / fftSize;
    return t;
}

//==============================================================================
Thd thd (const juce::AudioBuffer<float>& b, double sr, double f0)
{
    Thd r;
    const int n = b.getNumSamples();
    int order = 12;

    while ((1 << (order + 1)) <= n && order < 17)
        ++order;

    const int size = 1 << order;
    const int start = std::max (0, (n - size) / 2);
    auto power = powerSpectrum (b.getReadPointer (0), start, order);
    const double binHz = sr / size;

    auto peakAround = [&] (double hz, int span)
    {
        const int centre = (int) std::round (hz / binHz);
        double p = 0.0;
        int at = centre;

        for (int k = std::max (1, centre - span); k <= std::min ((int) power.size() - 1, centre + span); ++k)
            if (power[(size_t) k] > p)
            {
                p = power[(size_t) k];
                at = k;
            }

        // Sum the main lobe (Blackman-Harris: +-4 bins, two more for off-centre tones) so the
        // level is window-independent and nothing of the tone counts as "noise"
        double lobe = 0.0;

        for (int k = std::max (1, at - 6); k <= std::min ((int) power.size() - 1, at + 6); ++k)
            lobe += power[(size_t) k];

        return std::make_pair (lobe, at * binHz);
    };

    auto [fundPower, fundHz] = peakAround (f0, 4);
    r.fundamentalHz = fundHz;
    double harmonicPower = 0.0, evenPower = 0.0, oddPower = 0.0;

    for (int h = 2; h <= 12; ++h)
    {
        if (fundHz * h >= sr * 0.49)
            break;

        const double p = peakAround (fundHz * h, 3).first;
        harmonicPower += p;
        (h % 2 == 0 ? evenPower : oddPower) += p;
    }

    double total = 0.0;

    for (size_t k = 1; k < power.size(); ++k)
        total += power[k];

    const double rest = std::max (0.0, total - fundPower - harmonicPower);

    // Parseval: a sine of amplitude A leaves lobe power A^2 * N^2 * windowPowerGain / 4 in the one-sided spectrum
    const double scale = 4.0 / ((double) size * size * windowPowerGain);

    r.fundamentalDb = db (std::sqrt (fundPower * scale));
    r.thdPercent = fundPower > 0.0 ? 100.0 * std::sqrt (harmonicPower / fundPower) : 0.0;
    r.thdDb = fundPower > 0.0 ? 10.0 * std::log10 (std::max (1e-30, harmonicPower / fundPower)) : -200.0;
    r.thdnPercent = fundPower > 0.0 ? 100.0 * std::sqrt ((harmonicPower + rest) / fundPower) : 0.0;
    r.evenPercent = fundPower > 0.0 ? 100.0 * std::sqrt (evenPower / fundPower) : 0.0;
    r.oddPercent = fundPower > 0.0 ? 100.0 * std::sqrt (oddPower / fundPower) : 0.0;
    r.noiseFloorDb = db (std::sqrt (rest * scale));
    return r;
}

//==============================================================================
Decay decay (const juce::AudioBuffer<float>& b, double sr)
{
    Decay d;
    const int n = b.getNumSamples();
    auto* x = b.getReadPointer (0);

    float peak = 0.0f;
    int peakAt = 0;

    for (int i = 0; i < n; ++i)
        if (std::abs (x[i]) > peak)
        {
            peak = std::abs (x[i]);
            peakAt = i;
        }

    d.peakDb = db (peak);

    if (peak <= 0.0f)
        return d;

    // Schroeder backward integration from the peak
    std::vector<double> energy ((size_t) (n - peakAt), 0.0);
    double running = 0.0;

    for (int i = n - 1; i >= peakAt; --i)
    {
        running += (double) x[i] * x[i];
        energy[(size_t) (i - peakAt)] = running;
    }

    const double top = energy[0];
    auto timeToLevel = [&] (double dbDown) -> double
    {
        const double target = top * std::pow (10.0, -dbDown / 10.0);

        for (size_t i = 0; i < energy.size(); ++i)
            if (energy[i] <= target)
                return (double) i / sr;

        return (double) energy.size() / sr;
    };

    const double t5 = timeToLevel (5.0), t25 = timeToLevel (25.0), t35 = timeToLevel (35.0);
    d.t20 = (t25 - t5) * 3.0;
    d.t60 = (t35 - t5) * 2.0;

    auto centroid = [&] (int from, int length)
    {
        if (from + length > n || length < 256)
            return 0.0;

        int order = 8;

        while ((1 << (order + 1)) <= length && order < 15)
            ++order;

        auto p = powerSpectrum (x, from, order);
        double num = 0.0, den = 0.0;

        for (size_t k = 1; k < p.size(); ++k)
        {
            num += p[k] * k * sr / (1 << order);
            den += p[k];
        }

        return den > 0.0 ? num / den : 0.0;
    };

    d.earlyCentroidHz = centroid (peakAt, (int) (sr * 0.05));
    d.lateCentroidHz = centroid (peakAt + (int) (sr * 0.25), (int) (sr * 0.2));
    return d;
}

//==============================================================================
Bands bands (const juce::AudioBuffer<float>& b, double sr)
{
    Bands r;
    const int n = b.getNumSamples();
    const int order = 13, size = 1 << order;
    std::vector<double> sum ((size_t) size / 2 + 1, 0.0);
    int frames = 0;

    for (int start = 0; start + size <= n; start += size / 2)
    {
        auto p = powerSpectrum (b.getReadPointer (0), start, order);

        for (size_t k = 0; k < sum.size(); ++k)
            sum[k] += p[k];

        ++frames;
    }

    if (frames == 0)
        return r;

    const double binHz = sr / size;
    double total = 0.0, num = 0.0;
    double acc[6] {};
    const double edges[] = { 20.0, 60.0, 250.0, 1000.0, 4000.0, 10000.0, 24000.0 };

    for (size_t k = 1; k < sum.size(); ++k)
    {
        const double hz = k * binHz;
        total += sum[k];
        num += sum[k] * hz;

        for (int band = 0; band < 6; ++band)
            if (hz >= edges[band] && hz < edges[band + 1])
                acc[band] += sum[k];
    }

    if (total > 0.0)
    {
        r.sub = 100.0 * acc[0] / total;
        r.bass = 100.0 * acc[1] / total;
        r.lowMid = 100.0 * acc[2] / total;
        r.mid = 100.0 * acc[3] / total;
        r.high = 100.0 * acc[4] / total;
        r.air = 100.0 * acc[5] / total;
        r.centroidHz = num / total;
    }

    return r;
}

std::vector<std::pair<double, double>> responseAt (const juce::AudioBuffer<float>& b, double sr, const std::vector<double>& freqs)
{
    const int n = b.getNumSamples();
    const int order = 14, size = 1 << order;
    std::vector<double> sum ((size_t) size / 2 + 1, 0.0);
    int frames = 0;

    for (int start = 0; start + size <= n; start += size / 2)
    {
        auto p = powerSpectrum (b.getReadPointer (0), start, order);

        for (size_t k = 0; k < sum.size(); ++k)
            sum[k] += p[k];

        ++frames;
    }

    std::vector<std::pair<double, double>> out;
    const double binHz = sr / size;

    for (auto f : freqs)
    {
        const int lo = std::max (1, (int) std::round (f / std::pow (2.0, 1.0 / 6.0) / binHz));
        const int hi = std::min ((int) sum.size() - 1, (int) std::round (f * std::pow (2.0, 1.0 / 6.0) / binHz));
        double p = 0.0;

        for (int k = lo; k <= hi; ++k)
            p += sum[(size_t) k];

        out.emplace_back (f, frames > 0 ? 10.0 * std::log10 (std::max (1e-30, p / frames / (hi - lo + 1))) : -200.0);
    }

    return out;
}

//==============================================================================
juce::String describe (const Stats& s)
{
    return "peak " + juce::String (s.peakDb, 1) + " dBFS, rms " + juce::String (s.rmsDb, 1) + " dBFS, dc " + juce::String (s.dc, 5)
         + ", clipped " + juce::String (s.clipped) + (s.finite ? "" : ", NOT FINITE");
}

juce::String describe (const PitchTrack& t)
{
    return "mean " + juce::String (t.meanHz, 2) + " Hz, deviation " + juce::String (t.minCents, 1) + " .. +" + juce::String (t.maxCents, 1)
         + " cents (rms " + juce::String (t.rmsCents, 2) + "), modulation " + juce::String (t.modRateHz, 2) + " Hz over " + juce::String (t.windows) + " windows";
}

juce::String describe (const Thd& r)
{
    return "fundamental " + juce::String (r.fundamentalHz, 1) + " Hz at " + juce::String (r.fundamentalDb, 1) + " dBFS, THD " + juce::String (r.thdPercent, 3)
         + " % (" + juce::String (r.thdDb, 1) + " dB; even " + juce::String (r.evenPercent, 3) + " %, odd " + juce::String (r.oddPercent, 3)
         + " %), THD+N " + juce::String (r.thdnPercent, 3) + " %, residual " + juce::String (r.noiseFloorDb, 1) + " dBFS";
}

juce::String describe (const Decay& d)
{
    return "peak " + juce::String (d.peakDb, 1) + " dBFS, T60 " + juce::String (d.t60, 2) + " s (T20-based " + juce::String (d.t20, 2)
         + " s), centroid early " + juce::String ((int) d.earlyCentroidHz) + " Hz -> late " + juce::String ((int) d.lateCentroidHz) + " Hz";
}

juce::String describe (const Bands& b)
{
    return "sub " + juce::String (b.sub, 1) + " | bass " + juce::String (b.bass, 1) + " | lowmid " + juce::String (b.lowMid, 1) + " | mid "
         + juce::String (b.mid, 1) + " | high " + juce::String (b.high, 1) + " | air " + juce::String (b.air, 1) + " %, centroid "
         + juce::String ((int) b.centroidHz) + " Hz";
}

} // namespace measure
