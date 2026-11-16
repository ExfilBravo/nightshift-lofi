#include "LofiProcessor.h"

namespace lofi
{

LofiProcessor::LofiProcessor (Info i, Layout&& layout)
    : juce::AudioProcessor (BusesProperties().withInput ("Input", juce::AudioChannelSet::stereo(), true)
                                             .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "state", std::move (layout)),
      info (std::move (i))
{
    mixParam = apvts.getRawParameterValue ("mix");
    outputParam = apvts.getRawParameterValue ("output");
    bypassParam = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter ("bypass"));
    jassert (mixParam != nullptr && outputParam != nullptr && bypassParam != nullptr);   // forgot addCommonParameters?
}

float LofiProcessor::param (const juce::String& id) const
{
    if (auto* v = apvts.getRawParameterValue (id))
        return v->load();

    jassertfalse;
    return 0.0f;
}

//==============================================================================
void LofiProcessor::addCommonParameters (Layout& layout)
{
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID ("mix", 1), "Mix",
                                                             juce::NormalisableRange<float> (0.0f, 100.0f, 0.1f), 100.0f,
                                                             juce::AudioParameterFloatAttributes().withLabel ("%").withStringFromValueFunction (percentText)));
    layout.add (std::make_unique<juce::AudioParameterFloat> (juce::ParameterID ("output", 1), "Output",
                                                             juce::NormalisableRange<float> (-24.0f, 12.0f, 0.1f), 0.0f,
                                                             juce::AudioParameterFloatAttributes().withLabel ("dB").withStringFromValueFunction (dbText)));
    layout.add (std::make_unique<juce::AudioParameterBool> (juce::ParameterID ("bypass", 1), "Bypass", false));
}

juce::String LofiProcessor::percentText (float v, int)    { return juce::String (juce::roundToInt (v)) + " %"; }
juce::String LofiProcessor::dbText (float v, int)         { return (v > 0.0f ? "+" : "") + juce::String (v, 1) + " dB"; }
juce::String LofiProcessor::msText (float v, int)         { return juce::String (v, v < 10.0f ? 1 : 0) + " ms"; }

juce::String LofiProcessor::hzText (float v, int)
{
    return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String (v, v < 10.0f ? 2 : (v < 100.0f ? 1 : 0)) + " Hz";
}

//==============================================================================
bool LofiProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();
    return in == out && (in == juce::AudioChannelSet::mono() || in == juce::AudioChannelSet::stereo());
}

void LofiProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec { sampleRate, (juce::uint32) samplesPerBlock, (juce::uint32) std::max (1, getTotalNumOutputChannels()) };
    currentSampleRate = sampleRate;

    prepareDsp (spec);

    const int latency = getDspLatency();
    jassert (latency >= 0 && latency < maxLatencySamples);   // raise maxLatencySamples if a plugin needs more

    mixer.prepare (spec);
    mixer.setMixingRule (juce::dsp::DryWetMixingRule::linear);
    mixer.setWetLatency ((float) juce::jlimit (0, maxLatencySamples - 1, latency));
    setLatencySamples (latency);

    outputGain.reset (sampleRate, 0.02);
    outputGain.setCurrentAndTargetValue (juce::Decibels::decibelsToGain (outputParam->load()));

    transportGate.reset (sampleRate, 0.1);
    transportGate.setCurrentAndTargetValue (1.0f);
    gateFrom = gateTo = 1.0f;
    gatePrimed = false;

    primed = false;   // the first block picks up the bypass state without a fade

    for (auto& p : inputPeak)  p.store (0.0f);
    for (auto& p : outputPeak) p.store (0.0f);
}

void LofiProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    const int numChannels = std::min (buffer.getNumChannels(), getTotalNumOutputChannels());
    const int numSamples = buffer.getNumSamples();

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float peak = buffer.getMagnitude (ch, 0, numSamples);
        inputPeak[ch & 1].store (std::max (inputPeak[ch & 1].load(), peak));
    }

    const bool bypassRequested = bypassParam->get();
    const float mix = mixParam->load() * 0.01f;
    const float trim = juce::Decibels::decibelsToGain (outputParam->load());

    if (! primed)
    {
        primed = true;
        bypassState = bypassRequested ? BypassState::bypassed : BypassState::active;
        mixer.setWetMixProportion (bypassRequested ? 0.0f : mix);
        mixer.reset();
        outputGain.setCurrentAndTargetValue (bypassRequested ? 1.0f : trim);
    }
    else if (bypassRequested && bypassState == BypassState::active)
    {
        // Keep processing while the wet path fades out, then stop
        bypassState = BypassState::fadingOut;
        fadeRemaining = (int) std::ceil (currentSampleRate * 0.05) + 1;
    }
    else if (! bypassRequested && bypassState != BypassState::active)
    {
        // Back from bypass: start the wet path from silence and fade it in, while the
        // latency-matched dry keeps playing, so nothing from before the bypass is replayed
        if (bypassState == BypassState::bypassed)
            resetDsp();

        bypassState = BypassState::active;
    }

    const bool active = bypassState == BypassState::active;
    mixer.setWetMixProportion (active ? mix : 0.0f);
    outputGain.setTargetValue (active ? trim : 1.0f);

    // Made-up sound follows the host's transport, when it reports one: a stopped song is
    // silent, not still hissing (the first block lands on it, with no fade)
    float gateTarget = 1.0f;

    if (auto* head = getPlayHead())
        if (const auto position = head->getPosition())
            gateTarget = position->getIsPlaying() ? 1.0f : 0.0f;

    if (gatePrimed)
    {
        transportGate.setTargetValue (gateTarget);
    }
    else
    {
        transportGate.setCurrentAndTargetValue (gateTarget);
        gatePrimed = true;
    }

    gateFrom = transportGate.getCurrentValue();
    gateTo = transportGate.skip (numSamples);
    gateBlockLength = numSamples;

    juce::dsp::AudioBlock<float> block (buffer.getArrayOfWritePointers(), (size_t) numChannels, (size_t) numSamples);
    mixer.pushDrySamples (block);

    if (bypassState != BypassState::bypassed)
        processDsp (buffer);

    // Bypassed, the buffer still holds the input; at a wet proportion of 0 the mixer
    // returns the dry path alone, delayed by the reported latency
    mixer.mixWetSamples (block);

    if (outputGain.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = outputGain.getNextValue();

            for (int ch = 0; ch < numChannels; ++ch)
                buffer.getWritePointer (ch)[i] *= g;
        }
    }
    else if (outputGain.getCurrentValue() != 1.0f)
    {
        buffer.applyGain (0, numSamples, outputGain.getCurrentValue());
    }

    if (bypassState == BypassState::fadingOut && (fadeRemaining -= numSamples) <= 0)
        bypassState = BypassState::bypassed;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        const float peak = buffer.getMagnitude (ch, 0, numSamples);
        outputPeak[ch & 1].store (std::max (outputPeak[ch & 1].load(), peak));
    }
}

//==============================================================================
void LofiProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    auto state = apvts.copyState();
    state.setProperty ("pluginVersion", JucePlugin_VersionString, nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, dest);
}

void LofiProcessor::setStateInformation (const void* data, int size)
{
    if (auto xml = getXmlFromBinary (data, size))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

} // namespace lofi
