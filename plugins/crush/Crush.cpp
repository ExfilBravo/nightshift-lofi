#include "Crush.h"
#include "common/ScaledEditor.h"

using namespace lofi;
using namespace lofi::params;

namespace
{
    constexpr float maxDriveDb = 18.0f;        // drive 100
    constexpr float makeupFraction = 0.75f;    // how much of the drive is taken back after the converter

    // The longest the output filter rings (resonance 100 = Q 6 at 200 Hz falls 75 dB from
    // its peak in about 85 ms) and the DC blocker's release (10 Hz, 60 dB in 110 ms),
    // with margin; the converter's latency is added on top
    constexpr double ringSeconds = 0.15;

    float resonanceToQ (float resonance)
    {
        return 0.7f * std::pow (6.0f / 0.7f, juce::jlimit (0.0f, 100.0f, resonance) * 0.01f);
    }

    const char* const presetParams[] = { "bits", "rate", "alias", "filter" };

    /** "12.0 kHz" / "850 Hz": a cutoff, so one decimal is all the precision there is. */
    juce::String cutoffText (float v, int)
    {
        return v >= 1000.0f ? juce::String (v / 1000.0f, 1) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz";
    }
}

//==============================================================================
CrushProcessor::CrushProcessor()
    : LofiProcessor ({ "Nightshift Crush", "12-bit sampler", theme::violet }, createLayout())
{
    pBits      = apvts.getRawParameterValue ("bits");
    pRate      = apvts.getRawParameterValue ("rate");
    pDrive     = apvts.getRawParameterValue ("drive");
    pAlias     = apvts.getRawParameterValue ("alias");
    pFilter    = apvts.getRawParameterValue ("filter");
    pResonance = apvts.getRawParameterValue ("resonance");
    pMode      = apvts.getRawParameterValue ("mode");

    apvts.addParameterListener ("mode", this);

    for (auto* id : presetParams)
        apvts.addParameterListener (id, this);

    setTailSeconds (ringSeconds);
    startTimerHz (30);
}

CrushProcessor::~CrushProcessor()
{
    stopTimer();
    cancelPendingUpdate();
    apvts.removeParameterListener ("mode", this);

    for (auto* id : presetParams)
        apvts.removeParameterListener (id, this);
}

LofiProcessor::Layout CrushProcessor::createLayout()
{
    Layout layout;
    layout.add (choice  ("mode",      "Mode",      { "SP-1200", "MPC60", "Custom" }, 0));
    layout.add (integer ("bits", "Bits", 4, 16, 12, "bit", [] (int v, int) { return juce::String (v) + " bit"; }));
    layout.add (hertz   ("rate",      "Rate",      4000.0f, 48000.0f, 26040.0f, 16000.0f));
    layout.add (percent ("drive",     "Drive",     20.0f));
    layout.add (percent ("alias",     "Alias",     100.0f));
    layout.add (number  ("filter",    "Filter",    200.0f, 20000.0f, 0.01f, 12000.0f, "Hz", cutoffText, 2000.0f));
    layout.add (percent ("resonance", "Resonance", 20.0f));
    addCommonParameters (layout);
    return layout;
}

juce::AudioProcessorEditor* CrushProcessor::createEditor()
{
    return new lofi::ScaledEditor (*this, new CrushEditor (*this));
}

//==============================================================================
CrushProcessor::Preset CrushProcessor::preset (int mode)
{
    if (mode == 1)
        return { 12, 40000.0f, 60.0f, 16000.0f };    // MPC60

    return { 12, 26040.0f, 100.0f, 12000.0f };       // SP-1200
}

void CrushProcessor::setStateInformation (const void* data, int size)
{
    // A saved state already holds a consistent mode and knob set: restoring it must not
    // re-apply a preset over the saved knobs, nor flip the saved mode to Custom. The
    // restore notifies synchronously, so the guard covers it; anything still pending
    // from before the restore is dropped with it.
    restoringState = true;
    LofiProcessor::setStateInformation (data, size);
    restoringState = false;
    modeChangesApplied.store (modeChanges.load());
    knobsTouched = false;
}

void CrushProcessor::parameterChanged (const juce::String& id, float)
{
    // Called on whichever thread changed the parameter: the audio thread for host
    // automation. Only atomics here; the parameter writes happen in syncMode
    if (restoringState.load())
        return;

    if (id == "mode")
        modeChanges.fetch_add (1);
    else
        knobsTouched.store (true);

    // From the message thread (the editor, a host's generic UI) answer at once; the
    // timer picks up changes from anywhere else
    if (juce::MessageManager::existsAndIsCurrentThread())
        triggerAsyncUpdate();
}

void CrushProcessor::syncMode()
{
    const int changes = modeChanges.load();

    if (changes != modeChangesApplied.load())
    {
        const int mode = juce::roundToInt (pMode->load());

        if (mode >= 0 && mode < customMode)
        {
            const auto p = preset (mode);
            setParameter ("bits", (float) p.bits);
            setParameter ("rate", p.rate);
            setParameter ("alias", p.alias);
            setParameter ("filter", p.filter);
        }

        // The knobs now carry the preset, so the audio thread can follow them again
        modeChangesApplied.store (changes);
    }

    if (knobsTouched.exchange (false))
    {
        const int mode = juce::roundToInt (pMode->load());

        if (mode >= 0 && mode < customMode && ! knobsMatch (mode))
            setParameter ("mode", (float) customMode);
    }
}

void CrushProcessor::setParameter (const juce::String& id, float value)
{
    if (auto* p = apvts.getParameter (id))
    {
        const float normalised = p->convertTo0to1 (value);

        if (! juce::approximatelyEqual (p->getValue(), normalised))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (normalised);
            p->endChangeGesture();
        }
    }
}

bool CrushProcessor::knobsMatch (int mode) const
{
    const auto p = preset (mode);
    return std::abs (pBits->load() - (float) p.bits) < 0.5f
        && std::abs (pRate->load() - p.rate) < 1.0f
        && std::abs (pAlias->load() - p.alias) < 0.5f
        && std::abs (pFilter->load() - p.filter) < 1.0f;
}

//==============================================================================
void CrushProcessor::prepareDsp (const juce::dsp::ProcessSpec& spec)
{
    sampleRate = spec.sampleRate;
    maxBlock = std::max (1, (int) spec.maximumBlockSize);
    numChannels = (int) juce::jlimit (1u, 2u, spec.numChannels);

    // Linear-phase FIR halfbands (passband to 0.4 fs, 70 dB stopband): unlike the IIR
    // halfbands they leave the top octave's phase alone, so Mix below 100 % stays flat
    oversampling = std::make_unique<juce::dsp::Oversampling<float>> ((size_t) numChannels);
    oversampling->addOversamplingStage (juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
                                        0.1f, -70.0f, 0.1f, -70.0f);
    oversampling->setUsingIntegerLatency (true);
    oversampling->initProcessing ((size_t) maxBlock);
    oversampling->reset();

    for (int ch = 0; ch < 2; ++ch)
    {
        sampler[ch].prepare (sampleRate);
        outFilter[ch].reset();
        dcBlocker[ch].prepare (sampleRate);
    }

    latency = (int) std::round (oversampling->getLatencyInSamples()) + sampler[0].getLatency();
    setTailSeconds (ringSeconds + latency / sampleRate);

    lofi::dsp::prepareSmoothed (driveGain,   sampleRate, 1.0f);
    lofi::dsp::prepareSmoothed (makeupGain,  sampleRate, 1.0f);
    lofi::dsp::prepareSmoothed (aliasAmount, sampleRate, 1.0f);
    lofi::dsp::prepareSmoothed (rateHz,      sampleRate, 26040.0f, 0.03);
    lofi::dsp::prepareSmoothed (bitsValue,   sampleRate, 12.0f);
    lofi::dsp::prepareSmoothed (filterLog2,  sampleRate, std::log2 (12000.0f));
    lofi::dsp::prepareSmoothed (filterQ,     sampleRate, 1.0f);
    resetDsp();
}

void CrushProcessor::updateTargets()
{
    float bits = pBits->load(), rate = pRate->load(), alias = pAlias->load(), filter = pFilter->load();

    // A mode change the message thread has not yet written into the knobs: run on the
    // preset now, so automation of `mode` lands on its sample, offline renders included
    const int mode = juce::roundToInt (pMode->load());

    if (mode >= 0 && mode < customMode && modeChanges.load() != modeChangesApplied.load())
    {
        const auto p = preset (mode);
        bits = (float) p.bits;
        rate = p.rate;
        alias = p.alias;
        filter = p.filter;
    }

    const float driveDb = pDrive->load() * (maxDriveDb / 100.0f);
    driveGain.setTargetValue (lofi::dsp::dbToGain (driveDb));
    makeupGain.setTargetValue (lofi::dsp::dbToGain (-makeupFraction * driveDb));
    aliasAmount.setTargetValue (juce::jlimit (0.0f, 1.0f, alias * 0.01f));
    rateHz.setTargetValue (juce::jlimit (4000.0f, (float) sampleRate, rate));
    bitsValue.setTargetValue (juce::jlimit (4.0f, 16.0f, bits));
    filterLog2.setTargetValue (std::log2 (juce::jlimit (200.0f, 20000.0f, filter)));
    filterQ.setTargetValue (resonanceToQ (pResonance->load()));
}

void CrushProcessor::resetDsp()
{
    // Called on the audio thread when bypass ends: no allocation below
    updateTargets();

    for (auto* s : { &driveGain, &makeupGain, &aliasAmount, &rateHz, &bitsValue, &filterLog2, &filterQ })
        s->setCurrentAndTargetValue (s->getTargetValue());

    if (oversampling != nullptr)
        oversampling->reset();

    for (int ch = 0; ch < 2; ++ch)
    {
        sampler[ch].reset();
        outFilter[ch].reset();
        dcBlocker[ch].reset();
    }

    // Coefficients are recomputed from the snapped values on the next sample
    lastRate = lastBits = lastFc = lastQ = -1.0f;
}

void CrushProcessor::processDsp (juce::AudioBuffer<float>& buffer)
{
    const int channels = std::min (buffer.getNumChannels(), numChannels);
    const int total = buffer.getNumSamples();

    if (channels <= 0 || total <= 0 || oversampling == nullptr)
        return;

    // Targets once per call; the smoothers ramp sample by sample, so block size does not matter
    updateTargets();

    for (int start = 0; start < total; start += maxBlock)
    {
        const int n = std::min (maxBlock, total - start);
        processChunk (juce::dsp::AudioBlock<float> (buffer.getArrayOfWritePointers(), (size_t) channels, (size_t) start, (size_t) n));
    }
}

void CrushProcessor::processChunk (juce::dsp::AudioBlock<float> block)
{
    const int n = (int) block.getNumSamples();
    const int channels = (int) block.getNumChannels();

    // 1. Drive into the converter's input stage. The clip runs at 2x so its own harmonics
    //    above Nyquist are filtered away: the folding is the converter's job, not the clip's.
    for (int i = 0; i < n; ++i)
    {
        const float g = driveGain.getNextValue();

        for (int ch = 0; ch < channels; ++ch)
            block.getChannelPointer ((size_t) ch)[i] *= g;
    }

    {
        auto up = oversampling->processSamplesUp (block);
        const int upSamples = (int) up.getNumSamples();

        for (int ch = 0; ch < channels; ++ch)
        {
            float* d = up.getChannelPointer ((size_t) ch);

            for (int i = 0; i < upSamples; ++i)
                d[i] = crush::inputClip (d[i]);
        }

        oversampling->processSamplesDown (block);
    }

    // 2. The converter (sample, truncate, hold or rebuild), then DC block, makeup and
    //    the output filter.
    for (int i = 0; i < n; ++i)
    {
        // Alias as an amplitude taper: the first half of the knob is where aliasing
        // grows from inaudible to obvious, the second half where it takes over
        const float a = aliasAmount.getNextValue();
        const float flat = a * a;
        const float rate = rateHz.getNextValue();

        if (rate != lastRate)
        {
            lastRate = rate;

            for (int ch = 0; ch < channels; ++ch)
                sampler[ch].setRate (rate);
        }

        const float bits = bitsValue.getNextValue();

        if (bits != lastBits)
        {
            lastBits = bits;
            quantiser.setBits (bits);
        }

        const float fc = filterLog2.getNextValue();
        const float q = filterQ.getNextValue();

        if (fc != lastFc || q != lastQ)
        {
            lastFc = fc;
            lastQ = q;
            outCoefficients.set (std::exp2 ((double) fc), (double) q, sampleRate);
        }

        const float makeup = makeupGain.getNextValue();

        for (int ch = 0; ch < channels; ++ch)
        {
            float* d = block.getChannelPointer ((size_t) ch);
            float x = sampler[ch].process (d[i], flat, quantiser);
            x = dcBlocker[ch].process (x) * makeup;
            d[i] = outFilter[ch].process (x, outCoefficients);
        }
    }
}

//==============================================================================
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new CrushProcessor();
}
