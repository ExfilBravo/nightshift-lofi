// SPDX-License-Identifier: GPL-3.0-or-later
// NightShift - Copyright (C) 2026 NightWriter. See LICENSE.

#include "TextAudit.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <tuple>
#include <typeinfo>
#include <utility>

#if (defined (__GNUG__) || defined (__clang__)) && ! defined (_MSC_VER)   // clang-cl uses the MSVC library: no cxxabi.h
 #include <cstdlib>
 #include <cxxabi.h>
#endif

namespace nightshift::textaudit
{
namespace
{
    juce::String className (const juce::Component& c)
    {
        const char* raw = typeid (c).name();
        juce::String name (raw);

       #if (defined (__GNUG__) || defined (__clang__)) && ! defined (_MSC_VER)
        int status = 0;

        if (char* readable = abi::__cxa_demangle (raw, nullptr, nullptr, &status))
        {
            if (status == 0)
                name = readable;

            std::free (readable);
        }
       #endif

        for (auto prefix : { "class ", "struct " })
            if (name.startsWith (prefix))
                name = name.substring ((int) std::strlen (prefix));

        return name.replace ("nightshift::", "").replace ("juce::", "");
    }

    // The innermost shown component under a point (a snapshot's studio isn't on screen, so getComponentAt,
    // which asks whether it's visible and takes clicks, finds nothing)
    juce::Component& innermostAt (juce::Component& c, juce::Point<int> at)
    {
        for (int i = c.getNumChildComponents(); --i >= 0;)
            if (auto* child = c.getChildComponent (i); child->isVisible() && child->getBounds().contains (at))
                return innermostAt (*child, at - child->getPosition());

        return c;
    }

    // The component under a point, then up to two of its parents: enough to find the code that drew it
    juce::String ownerOf (juce::Component& root, juce::Point<int> at)
    {
        juce::StringArray path;

        for (auto* c = &innermostAt (root, at); c != nullptr && c != &root && path.size() < 3; c = c->getParentComponent())
            path.add (className (*c));

        return path.isEmpty() ? className (root) : path.joinIntoString (" < ");
    }

    // A typeface's glyph numbers back to the characters they draw, with each one's width at 1 px
    struct Glyph
    {
        juce_wchar character;
        float advance;
    };

    const std::map<int, Glyph>& glyphsOf (const juce::Font& font)
    {
        static std::map<juce::String, std::map<int, Glyph>> typefaces;
        auto [entry, isNew] = typefaces.try_emplace (font.getTypefaceName() + "|" + font.getTypefaceStyle());

        if (isNew)
        {
            juce::String all;

            for (int c = 32; c < 127; ++c)
                all << juce::String::charToString ((juce_wchar) c);

            // The rest NightShift draws: dots, dashes, quotes, arrows, the Mac's keys, a tick and a cross
            for (int c : { 0xb7, 0x2022, 0x2013, 0x2014, 0x2018, 0x2019, 0x201c, 0x201d, 0x2026, 0xd7, 0xb0, 0xb1, 0xe9,
                           0x266a, 0x25b6, 0x2190, 0x2191, 0x2192, 0x2193, 0x2318, 0x2325, 0x21e7, 0x2713, 0x2715, 0x2212 })
                all << juce::String::charToString ((juce_wchar) c);

            juce::GlyphArrangement probe;
            probe.addLineOfText (font.withHeight (100.0f).withHorizontalScale (1.0f).withExtraKerningFactor (0.0f), all, 0.0f, 0.0f);

            for (int i = 0; i < probe.getNumGlyphs(); ++i)
            {
                const auto& g = probe.getGlyph (i);
                entry->second.try_emplace (g.getGlyphIndex(), Glyph { g.getCharacter(), (g.getRight() - g.getLeft()) / 100.0f });
            }
        }

        return entry->second;
    }

    // The software renderer, keeping track of where it's drawing so each run of glyphs can be read back
    class Recorder : public juce::LowLevelGraphicsSoftwareRenderer
    {
    public:
        Recorder (const juce::Image& image, juce::Component& rootToPaint, std::vector<Run>& into)
            : LowLevelGraphicsSoftwareRenderer (image), root (rootToPaint), runs (into)
        {
        }

        // JUCE paints a child by clipping to it (what of it is in view), then moving the origin to its corner,
        // children in order: following that says which component is painting now, so text can be held to that
        // component's own edges, all of it, not just the part in view
        bool clipToRectangle (const juce::Rectangle<int>& r) override
        {
            const bool any = LowLevelGraphicsSoftwareRenderer::clipToRectangle (r);
            lastClip = r;
            return any;
        }

        void setOrigin (juce::Point<int> o) override
        {
            LowLevelGraphicsSoftwareRenderer::setOrigin (o);
            transform = juce::AffineTransform::translation ((float) o.x, (float) o.y).followedBy (transform);

            if (lastClip.has_value() && current != nullptr)
            {
                auto& next = nextChild[current];

                for (int i = next; i < current->getNumChildComponents(); ++i)
                    if (auto* child = current->getChildComponent (i); child->isVisible() && child->getPosition() == o)
                    {
                        next = i + 1;
                        current = child;
                        nextChild[child] = 0;
                        ownBounds = child->getLocalBounds().toFloat();
                        break;
                    }
            }

            lastClip.reset();
        }

        void addTransform (const juce::AffineTransform& t) override
        {
            LowLevelGraphicsSoftwareRenderer::addTransform (t);
            transform = t.followedBy (transform);
            lastClip.reset();
        }

        void saveState() override
        {
            LowLevelGraphicsSoftwareRenderer::saveState();
            saved.push_back ({ transform, fill, ownBounds, current });
        }

        void restoreState() override
        {
            LowLevelGraphicsSoftwareRenderer::restoreState();
            lastClip.reset();

            if (! saved.empty())
            {
                std::tie (transform, fill, ownBounds, current) = saved.back();
                saved.pop_back();
            }
        }

        // The colour text is drawn in: a gradient counts as its first colour. (A colour fill's opacity is its
        // colour's alpha; a gradient's is kept in that alpha too, and setOpacity replaces it.)
        void setFill (const juce::FillType& f) override
        {
            LowLevelGraphicsSoftwareRenderer::setFill (f);
            fill = f.isColour() ? f.colour
                 : f.isGradient() && f.gradient->getNumColours() > 0 ? f.gradient->getColour (0).withMultipliedAlpha (f.getOpacity())
                                                                     : juce::Colours::white.withAlpha (f.getOpacity());
        }

        void setOpacity (float opacity) override
        {
            LowLevelGraphicsSoftwareRenderer::setOpacity (opacity);
            fill = fill.withAlpha (opacity);
        }

        // A component faded with setAlpha (a greyed-out knob) is painted into a see-through layer: its text
        // comes out that much fainter than the colour it's drawn in
        void beginTransparencyLayer (float opacity) override
        {
            LowLevelGraphicsSoftwareRenderer::beginTransparencyLayer (opacity);
            layers.push_back ((layers.empty() ? 1.0f : layers.back()) * opacity);
        }

        void endTransparencyLayer() override
        {
            LowLevelGraphicsSoftwareRenderer::endTransparencyLayer();

            if (! layers.empty())
                layers.pop_back();
        }

        void drawGlyphs (juce::Span<const uint16_t> glyphs, juce::Span<const juce::Point<float>> positions,
                         const juce::AffineTransform& t) override
        {
            LowLevelGraphicsSoftwareRenderer::drawGlyphs (glyphs, positions, t);

            if (glyphs.empty() || positions.size() < glyphs.size())
                return;

            const auto font = getFont();
            const auto& known = glyphsOf (font);
            const float px = font.getHeight(), ascent = font.getAscent(), descent = font.getDescent();
            const float across = px * font.getHorizontalScale();

            Run run;
            juce::Rectangle<float> box;
            float right = positions[0].x, baseline = positions[0].y;

            for (size_t i = 0; i < glyphs.size(); ++i)
            {
                const auto found = known.find ((int) glyphs[i]);
                const float advance = found != known.end() ? found->second.advance * across : px * 0.5f;
                const auto p = positions[i];

                // Spaces aren't drawn, so a gap is one; a step down is a new line
                if (i > 0 && std::abs (p.y - baseline) > px * 0.5f)
                    run.text << " / ";
                else if (i > 0 && p.x - right > px * 0.18f)
                    run.text << " ";

                run.text << (found != known.end() ? juce::String::charToString (found->second.character) : juce::String ("?"));

                const juce::Rectangle<float> glyphBox (p.x, p.y - ascent, advance, ascent + descent);
                box = i == 0 ? glyphBox : box.getUnion (glyphBox);
                right = p.x + advance;
                baseline = p.y;
            }

            const auto full = t.followedBy (transform);
            run.height = px * std::sqrt (std::abs (full.mat00 * full.mat11 - full.mat01 * full.mat10));
            run.squash = font.getHorizontalScale();
            run.capitals = font.getExtraKerningFactor() > 0.03f;
            run.cut = run.text.containsChar ((juce_wchar) 0x2026);
            run.theirs = TheirWords::depth() > 0;
            run.colour = fill.withMultipliedAlpha (layers.empty() ? 1.0f : layers.back());

            // Greyed out: drawn through a see-through layer (JUCE's only use of one is a faded component), or
            // by a switched-off component
            run.inactive = ! layers.empty() && layers.back() < 0.99f;

            for (auto* c = current; c != nullptr; c = c->getParentComponent())
                run.inactive = run.inactive || ! c->isEnabled();

            if (current != nullptr)
                run.drawnBy = className (*current) + (current->getParentComponent() != nullptr ? " < " + className (*current->getParentComponent()) : juce::String());
            run.area = box.transformedBy (full);
            run.owner = ownerOf (root, run.area.getCentre().roundToInt());

            // Text nobody sees (JUCE keeps a clear label in its dialogs for screen readers) isn't checked
            if (fill.getFloatAlpha() < 0.02f)
                return;

            // Clipped: running out of the component that drew it. (Not just out of view: a strip scrolled half
            // under the mixer's Master is scrolling, not text that doesn't fit.)
            run.clipped = ! ownBounds.expanded (1.0f).contains (box.transformedBy (t));
            run.within = ownBounds.transformedBy (transform);
            runs.push_back (std::move (run));
        }

    private:
        juce::Component& root;
        std::vector<Run>& runs;
        juce::AffineTransform transform;
        juce::Colour fill { juce::Colours::black };
        juce::Rectangle<float> ownBounds { root.getLocalBounds().toFloat() };
        juce::Component* current = &root;
        std::map<juce::Component*, int> nextChild;
        std::optional<juce::Rectangle<int>> lastClip;
        std::vector<float> layers;
        std::vector<std::tuple<juce::AffineTransform, juce::Colour, juce::Rectangle<float>, juce::Component*>> saved;
    };

    float luminance (juce::Colour c)
    {
        auto channel = [] (juce::uint8 v)
        {
            const float s = (float) v / 255.0f;
            return s <= 0.04045f ? s / 12.92f : std::pow ((s + 0.055f) / 1.055f, 2.4f);
        };

        return 0.2126f * channel (c.getRed()) + 0.7152f * channel (c.getGreen()) + 0.0722f * channel (c.getBlue());
    }

    // What's behind a run: the middle of its box's pixels by brightness (the letters cover well under half)
    // and the text's colour laid over that, compared as WCAG does
    void measureContrast (Run& run, const juce::Image& image)
    {
        const auto box = run.area.getSmallestIntegerContainer().getIntersection (image.getBounds());

        if (box.isEmpty())
            return;

        std::vector<std::pair<float, juce::Colour>> pixels;
        const juce::Image::BitmapData data (image, juce::Image::BitmapData::readOnly);

        for (int y = box.getY(); y < box.getBottom(); y += 1)
            for (int x = box.getX(); x < box.getRight(); x += 1)
            {
                const auto c = data.getPixelColour (x, y);
                pixels.push_back ({ luminance (c), c });
            }

        std::nth_element (pixels.begin(), pixels.begin() + (long) pixels.size() / 2, pixels.end(),
                          [] (const auto& a, const auto& b) { return a.first < b.first; });
        const auto behind = pixels[pixels.size() / 2].second.withAlpha (1.0f);
        const float lText = luminance (behind.interpolatedWith (run.colour.withAlpha (1.0f), run.colour.getFloatAlpha()));
        const float lBehind = luminance (behind);
        run.contrast = (std::max (lText, lBehind) + 0.05f) / (std::min (lText, lBehind) + 0.05f);
    }
}

std::vector<Run> collect (juce::Component& c, juce::Image* imageOut)
{
    std::vector<Run> runs;
    juce::Image image (juce::Image::ARGB, std::max (1, c.getWidth()), std::max (1, c.getHeight()), true, juce::SoftwareImageType());

    {
        Recorder recorder (image, c, runs);
        juce::Graphics g (recorder);
        c.paintEntireComponent (g, true);
    }

    for (auto& run : runs)
        measureContrast (run, image);

    if (imageOut != nullptr)
        *imageOut = image;

    return runs;
}

juce::String report (const std::vector<Run>& runs, bool all)
{
    int small = 0, squeezed = 0, cut = 0, clipped = 0, faint = 0;
    juce::StringArray lines;
    std::map<juce::String, int> repeats;

    for (const auto& r : runs)
    {
        juce::StringArray kinds;

        if (r.small())     { kinds.add ("SMALL"); ++small; }
        if (r.squeezed())  { kinds.add ("SQUEEZED"); ++squeezed; }
        if (r.cutShort())  { kinds.add ("CUT"); ++cut; }
        if (r.clipped)     { kinds.add ("CLIPPED"); ++clipped; }
        if (r.faint())     { kinds.add ("FAINT"); ++faint; }

        if (kinds.isEmpty() && ! all)
            continue;

        const auto line = (kinds.isEmpty() ? juce::String ("ok") : kinds.joinIntoString ("+"))
                        + "  " + juce::String (r.height, 1) + " px" + (r.squeezed() ? " x" + juce::String (r.squash, 2) : juce::String())
                        + (r.capitals ? " caps" : "") + (r.inactive ? " off" : "") + "  " + juce::String (r.contrast, 1) + ":1 #" + r.colour.toDisplayString (true)
                        + "  \"" + r.text + "\"  in " + r.owner + "  by " + r.drawnBy;

        if (++repeats[line] == 1)
            lines.add (line + "  at " + juce::String (juce::roundToInt (r.area.getX())) + "," + juce::String (juce::roundToInt (r.area.getY()))
                       + (r.clipped ? "  (" + juce::String (juce::roundToInt (r.area.getWidth())) + "x" + juce::String (juce::roundToInt (r.area.getHeight()))
                                        + " in " + r.within.toNearestInt().toString() + ")" : juce::String()));
    }

    juce::String out;

    for (auto line : lines)
    {
        const int n = repeats[line.upToLastOccurrenceOf ("  at ", false, false)];
        out << line << (n > 1 ? "  (x" + juce::String (n) + ")" : juce::String()) << "\n";
    }

    out << "TEXT " << (int) runs.size() << " runs: " << small << " small, " << squeezed << " squeezed, "
        << cut << " cut, " << clipped << " clipped, " << faint << " faint\n";
    return out;
}

} // namespace nightshift::textaudit
