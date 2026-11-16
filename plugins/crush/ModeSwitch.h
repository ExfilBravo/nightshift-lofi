#pragma once

#include <JuceHeader.h>
#include "common/LofiTheme.h"

/** The converter mode as three latching push buttons with violet LEDs (SP-1200, MPC60,
    CUSTOM), bound to the `mode` choice parameter. The selected one sits down with its LED
    lit; the others stand proud and brighten on hover. Drawn like the shared LedButton so
    it reads as hardware next to Cassette's slide switch and Vinyl's 33 / 45 buttons. */
class ModeSwitch : public juce::Component,
                   public juce::SettableTooltipClient
{
public:
    ModeSwitch (juce::AudioProcessorValueTreeState& state, const juce::String& paramId)
        : parameter (*state.getParameter (paramId)),
          attachment (parameter, [this] (float v) { current = juce::roundToInt (v); repaint(); })
    {
        if (auto* choice = dynamic_cast<juce::AudioParameterChoice*> (&parameter))
            for (auto& name : choice->choices)
                names.add (name.toUpperCase());

        setTooltip ("Converter model: SP-1200 and MPC60 set bits, rate, alias and filter; moving any of those makes it CUSTOM");
        attachment.sendInitialUpdate();
    }

    void paint (juce::Graphics& g) override
    {
        using namespace lofi::theme;

        for (int i = 0; i < names.size(); ++i)
        {
            const bool on = i == current;
            auto b = buttonArea (i);

            if (on)
            {
                b = b.translated (0.0f, 1.0f);      // latched down: no shadow, darker cap
            }
            else
            {
                g.setColour (juce::Colours::black.withAlpha (0.5f));
                g.fillRoundedRectangle (b.translated (0.0f, 1.5f), 4.0f);
            }

            auto top = juce::Colour (on ? 0xff1f232c : 0xff2c313d), bottom = juce::Colour (on ? 0xff191c24 : 0xff222631);

            if (i == hover && ! on)
            {
                top = top.brighter (0.07f);
                bottom = bottom.brighter (0.07f);
            }

            g.setGradientFill (juce::ColourGradient (top, b.getX(), b.getY(), bottom, b.getX(), b.getBottom(), false));
            g.fillRoundedRectangle (b, 4.0f);
            g.setColour (outline);
            g.drawRoundedRectangle (b.reduced (0.5f), 4.0f, 1.0f);
            g.setColour (on ? juce::Colours::black.withAlpha (0.35f) : juce::Colours::white.withAlpha (0.07f));
            g.drawHorizontalLine ((int) b.getY() + 1, b.getX() + 3.0f, b.getRight() - 3.0f);

            auto led = juce::Rectangle<float> (6.0f, 6.0f).withCentre ({ b.getX() + 10.0f, b.getCentreY() });

            if (on)
            {
                g.setColour (violet.withAlpha (0.3f));
                g.fillEllipse (led.expanded (2.5f));
            }

            g.setColour (on ? violet.brighter (0.25f) : violet.withAlpha (0.16f));
            g.fillEllipse (led);

            g.setColour (on ? text : textDim.brighter (i == hover ? 0.4f : 0.15f));
            g.setFont (font (11.5f, true).withExtraKerningFactor (0.04f));
            g.drawText (names[i], b.withTrimmedLeft (17.0f).withTrimmedRight (3.0f), juce::Justification::centred, true);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        const int i = buttonAt (e.position);

        if (i >= 0 && i != current)
            attachment.setValueAsCompleteGesture ((float) i);
    }

    void mouseMove (const juce::MouseEvent& e) override    { setHover (buttonAt (e.position)); }
    void mouseExit (const juce::MouseEvent&) override      { setHover (-1); }

private:
    juce::Rectangle<float> buttonArea (int index) const
    {
        const int count = std::max (1, names.size());
        const float gap = 6.0f;
        const auto r = getLocalBounds().toFloat().reduced (0.0f, 1.0f).withTrimmedBottom (1.0f);
        const float w = (r.getWidth() - gap * (float) (count - 1)) / (float) count;
        return { r.getX() + (float) index * (w + gap), r.getY(), w, r.getHeight() };
    }

    int buttonAt (juce::Point<float> p) const
    {
        for (int i = 0; i < names.size(); ++i)
            if (buttonArea (i).expanded (2.0f).contains (p))
                return i;

        return -1;
    }

    void setHover (int index)
    {
        if (index == hover)
            return;

        hover = index;
        setMouseCursor (index >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
        repaint();
    }

    juce::RangedAudioParameter& parameter;
    juce::ParameterAttachment attachment;
    juce::StringArray names;
    int current = 0, hover = -1;
};
