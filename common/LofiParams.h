#pragma once

#include <JuceHeader.h>
#include "LofiProcessor.h"

/** Short-hands for declaring parameters, so each plugin's list reads like a spec. */
namespace lofi::params
{

using Text = std::function<juce::String (float, int)>;

inline std::unique_ptr<juce::AudioParameterFloat> number (const char* id, const char* name, float min, float max,
                                                          float step, float def, const char* unit, Text text = nullptr,
                                                          float skewMidpoint = 0.0f)
{
    juce::NormalisableRange<float> range (min, max, step);

    if (skewMidpoint > 0.0f)
        range.setSkewForCentre (skewMidpoint);

    auto attributes = juce::AudioParameterFloatAttributes().withLabel (unit);

    if (text != nullptr)
        attributes = attributes.withStringFromValueFunction (text);

    return std::make_unique<juce::AudioParameterFloat> (juce::ParameterID (id, 1), name, range, def, attributes);
}

inline std::unique_ptr<juce::AudioParameterFloat> percent (const char* id, const char* name, float def)
{
    return number (id, name, 0.0f, 100.0f, 0.1f, def, "%", LofiProcessor::percentText);
}

inline std::unique_ptr<juce::AudioParameterFloat> decibels (const char* id, const char* name, float min, float max, float def)
{
    return number (id, name, min, max, 0.1f, def, "dB", LofiProcessor::dbText);
}

inline std::unique_ptr<juce::AudioParameterFloat> hertz (const char* id, const char* name, float min, float max, float def, float skewMidpoint)
{
    return number (id, name, min, max, 0.01f, def, "Hz", LofiProcessor::hzText, skewMidpoint);
}

inline std::unique_ptr<juce::AudioParameterChoice> choice (const char* id, const char* name, juce::StringArray choices, int def)
{
    return std::make_unique<juce::AudioParameterChoice> (juce::ParameterID (id, 1), name, std::move (choices), def);
}

/** juce::AudioParameterInt does not report itself as discrete, so a VST3 host would see a
    continuous parameter, write values between the steps and drift from the plugin's view. */
struct SteppedInt : juce::AudioParameterInt
{
    using juce::AudioParameterInt::AudioParameterInt;
    bool isDiscrete() const override    { return true; }
};

using IntText = std::function<juce::String (int, int)>;

inline std::unique_ptr<juce::AudioParameterInt> integer (const char* id, const char* name, int min, int max, int def,
                                                         const char* unit = "", IntText text = nullptr)
{
    auto attributes = juce::AudioParameterIntAttributes().withLabel (unit);

    if (text != nullptr)
        attributes = attributes.withStringFromValueFunction (text);

    return std::make_unique<SteppedInt> (juce::ParameterID (id, 1), name, min, max, def, attributes);
}

inline std::unique_ptr<juce::AudioParameterBool> toggle (const char* id, const char* name, bool def)
{
    return std::make_unique<juce::AudioParameterBool> (juce::ParameterID (id, 1), name, def);
}

} // namespace lofi::params
