// SPDX-License-Identifier: GPL-3.0-or-later
// NightShift - Copyright (C) 2026 NightWriter. See LICENSE.

#pragma once

#include <JuceHeader.h>
#include <vector>

/** The text check: paints a window the way a snapshot does and notes every piece of text it draws - what it
    says, how big it came out, and whether it fit. The rule (the user, 2026-11-15: "make all of the text more
    readable"): nothing anyone reads under 15 px (the spaced-out small capitals over readouts and sections: 14),
    and nothing squeezed sideways or cut short to fit. */
namespace nightshift::textaudit
{
    constexpr float floorPx = 15.0f, capitalsFloorPx = 14.0f;

    /** Text against what's behind it, as WCAG measures it: 4.5 to 1 is the least for text this size. A
        greyed-out control's words may be dimmer, to look switched off, but never under 3 to 1. */
    constexpr float leastContrast = 4.5f, leastInactiveContrast = 3.0f;

    struct Run
    {
        juce::String text;              // what it says, as near as its glyphs tell
        float height = 0.0f;            // the font's height, times any scale it was drawn at
        float squash = 1.0f;            // under 1: squeezed sideways to fit
        bool capitals = false;          // the spaced-out small capitals (theme::labelFont)
        bool inactive = false;          // on a greyed-out (disabled or faded) control
        bool theirs = false;            // the user's own words (TheirWords): cut short is fine
        bool cut = false;               // ends in "...": the rest didn't fit
        bool clipped = false;           // runs past the edge it's drawn inside
        juce::Colour colour;            // what it was drawn in, its opacity included
        float contrast = 21.0f;         // against what's behind it
        juce::Rectangle<float> area;    // where, in the painted component
        juce::Rectangle<float> within;  // the component that drew it, in the painted component
        juce::String owner;             // the component under it, then its parents
        juce::String drawnBy;           // the component that drew it, then its parent

        bool small() const      { return height < (capitals ? capitalsFloorPx : floorPx) - 0.01f; }
        bool squeezed() const   { return squash < 0.99f; }
        bool faint() const      { return contrast < (inactive ? leastInactiveContrast : leastContrast) - 0.01f; }
        bool cutShort() const   { return cut && ! theirs; }
        bool fine() const       { return ! small() && ! squeezed() && ! cutShort() && ! clipped && ! faint(); }
    };

    /** Held while drawing the user's own words (a channel's name, a sound's, a project's folder), which may be
        cut short to fit a narrow strip: the check lets those end in "...". Ours never may. */
    struct TheirWords
    {
        TheirWords() noexcept                   { ++depth(); }
        ~TheirWords() noexcept                  { --depth(); }
        static int& depth() noexcept            { static int held = 0; return held; }
    };

    /** Paints the component and everything in it, as createComponentSnapshot does, noting every run of text.
        The picture it painted goes in imageOut. */
    std::vector<Run> collect (juce::Component&, juce::Image* imageOut = nullptr);

    /** A line for each run that's too small, squeezed, cut, clipped or faint (with all, every run; the same text
        in the same place once, with how many), then a count: "TEXT n runs: a small, b squeezed, c cut, d clipped,
        e faint". */
    juce::String report (const std::vector<Run>&, bool all = false);
}
