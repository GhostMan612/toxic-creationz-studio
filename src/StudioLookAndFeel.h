#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>

class StudioLookAndFeel : public juce::LookAndFeel_V4
{
public:
    StudioLookAndFeel()
    {
        setColour (juce::ResizableWindow::backgroundColourId, juce::Colour (0xff12151c));
        setColour (juce::TextButton::buttonColourId,          juce::Colour (0xff232937));
        setColour (juce::TextButton::buttonOnColourId,        juce::Colour (0xff0ea5e9));
        setColour (juce::TextButton::textColourOffId,         juce::Colour (0xffe2e8f0));
        setColour (juce::TextButton::textColourOnId,          juce::Colours::white);

        setColour (juce::ComboBox::backgroundColourId,        juce::Colour (0xff1c212d));
        setColour (juce::ComboBox::outlineColourId,           juce::Colour (0xff334155));
        setColour (juce::ComboBox::textColourId,              juce::Colour (0xffe2e8f0));
        setColour (juce::ComboBox::arrowColourId,             juce::Colour (0xff94a3b8));

        setColour (juce::PopupMenu::backgroundColourId,            juce::Colour (0xff1b202b));
        setColour (juce::PopupMenu::textColourId,                  juce::Colour (0xffe2e8f0));
        setColour (juce::PopupMenu::highlightedBackgroundColourId, juce::Colour (0xff0284c7));

        setColour (juce::Slider::backgroundColourId,          juce::Colour (0xff1b202c));
        setColour (juce::Slider::trackColourId,               juce::Colour (0xff38bdf8));
        setColour (juce::Slider::thumbColourId,               juce::Colour (0xfff8fafc));
        setColour (juce::Slider::textBoxTextColourId,         juce::Colour (0xffe2e8f0));
        setColour (juce::Slider::textBoxBackgroundColourId,   juce::Colour (0xff181c26));
        setColour (juce::Slider::textBoxOutlineColourId,      juce::Colour (0xff2e3646));

        setColour (juce::Label::textColourId,                 juce::Colour (0xffe6edf3));
        setColour (juce::ScrollBar::thumbColourId,            juce::Colour (0xff475569));

        // WO-022B boutique hardware palette.
        setColour (juce::TextButton::buttonOnColourId,        juce::Colour (0xff2dff8f));
        setColour (juce::Slider::trackColourId,               juce::Colour (0xff2dff8f));
    }

    static constexpr juce::uint32 chassisColour    = 0xff111317;
    static constexpr juce::uint32 bezelColour      = 0xff1a1e24;
    static constexpr juce::uint32 panelEdgeColour  = 0xff2b313c;
    static constexpr juce::uint32 emeraldColour    = 0xff2dff8f;
    static constexpr juce::uint32 amberColour      = 0xffffb020;
    static constexpr juce::uint32 crimsonColour    = 0xffff3b5c;
    static constexpr juce::uint32 labelColour      = 0xffe6edf3;
    static constexpr juce::uint32 mutedLabelColour = 0xff8b949e;

    void drawButtonBackground (juce::Graphics& g,
                               juce::Button& button,
                               const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override
    {
        const auto bounds = button.getLocalBounds().toFloat().reduced (0.5f);
        auto col = backgroundColour;

        if (shouldDrawButtonAsDown)
            col = col.brighter (0.14f);
        else if (shouldDrawButtonAsHighlighted)
            col = col.brighter (0.07f);

        if (! button.isEnabled())
            col = col.withAlpha (0.45f);

        g.setColour (col);
        g.fillRoundedRectangle (bounds, 4.0f);

        g.setColour (button.getToggleState()
                         ? juce::Colour (emeraldColour).withAlpha (0.75f)
                         : juce::Colour (mutedLabelColour));
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);

        // Recessed LED status dot on the right edge of the chassis button.
        const float ledDiameter = juce::jmin (6.0f, bounds.getHeight() * 0.28f);
        const auto ledBounds = juce::Rectangle<float> (
            bounds.getRight() - ledDiameter - 5.0f,
            bounds.getCentreY() - ledDiameter * 0.5f,
            ledDiameter, ledDiameter);
        g.setColour (button.getToggleState()
                         ? button.findColour (juce::TextButton::buttonOnColourId)
                         : juce::Colour (panelEdgeColour));
        g.fillEllipse (ledBounds);
        g.setColour (juce::Colour (chassisColour).withAlpha (0.9f));
        g.drawEllipse (ledBounds, 1.0f);
    }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider& slider) override
    {
        const auto area = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y),
                                                  static_cast<float> (width), static_cast<float> (height));
        const float diameter = juce::jmin (area.getWidth(), area.getHeight()) - 4.0f;
        if (diameter <= 8.0f)
            return;

        const auto centre = area.getCentre();
        const float radius = diameter * 0.42f;
        const float angle = rotaryStartAngle
            + (rotaryEndAngle - rotaryStartAngle) * juce::jlimit (0.0f, 1.0f, sliderPosProportional);

        juce::Path arc;
        arc.addArc (centre.x - radius, centre.y - radius, radius * 2.0f, radius * 2.0f,
                    rotaryStartAngle, angle, true);
        g.setColour (slider.findColour (juce::Slider::trackColourId));
        g.strokePath (arc, juce::PathStrokeType (2.5f, juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));

        for (int i = 0; i <= 10; ++i)
        {
            const float a = rotaryStartAngle
                + (rotaryEndAngle - rotaryStartAngle) * (static_cast<float> (i) / 10.0f);
            const auto p1 = centre + juce::Point<float> (std::cos (a), std::sin (a)) * (radius + 4.0f);
            const auto p2 = centre + juce::Point<float> (std::cos (a), std::sin (a)) * (radius + 8.0f);
            g.setColour (juce::Colour (mutedLabelColour).withAlpha (0.6f));
            g.drawLine (p1.x, p1.y, p2.x, p2.y, 1.0f);
        }

        g.setColour (juce::Colour (panelEdgeColour));
        g.fillEllipse (centre.x - radius * 0.82f, centre.y - radius * 0.82f,
                       radius * 1.64f, radius * 1.64f);
        g.setColour (juce::Colour (bezelColour));
        g.fillEllipse (centre.x - radius * 0.72f, centre.y - radius * 0.72f,
                       radius * 1.44f, radius * 1.44f);
        g.setColour (juce::Colour (labelColour).withAlpha (0.9f));
        g.drawLine (centre.x + std::cos (angle) * radius * 0.20f,
                    centre.y + std::sin (angle) * radius * 0.20f,
                    centre.x + std::cos (angle) * radius * 0.68f,
                    centre.y + std::sin (angle) * radius * 0.68f, 2.0f);
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                           float sliderPos, float, float,
                           const juce::Slider::SliderStyle style, juce::Slider& slider) override
    {
        const bool vertical = style == juce::Slider::LinearVertical
                           || style == juce::Slider::LinearBarVertical;
        const auto area = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y),
                                                  static_cast<float> (width), static_cast<float> (height));
        g.setColour (juce::Colour (chassisColour));
        g.fillRoundedRectangle (area, 3.0f);

        const auto groove = vertical
            ? area.withSizeKeepingCentre (juce::jmax (6.0f, area.getWidth() * 0.22f), area.getHeight() - 12.0f)
            : area.withSizeKeepingCentre (area.getWidth() - 12.0f, juce::jmax (6.0f, area.getHeight() * 0.22f));
        g.setColour (juce::Colour (panelEdgeColour));
        g.fillRoundedRectangle (groove, 3.0f);

        for (int i = 0; i <= 10; ++i)
        {
            const float frac = static_cast<float> (i) / 10.0f;
            const auto tick = vertical
                ? juce::Point<float> (groove.getRight() + 3.0f, groove.getY() + groove.getHeight() * frac)
                : juce::Point<float> (groove.getX() + groove.getWidth() * frac, groove.getBottom() + 3.0f);
            g.setColour (juce::Colour (mutedLabelColour).withAlpha (0.5f));
            g.drawLine (tick.x, tick.y, tick.x + (vertical ? 4.0f : 0.0f), tick.y + (vertical ? 0.0f : 3.0f), 1.0f);
        }

        g.setColour (slider.findColour (juce::Slider::thumbColourId));
        if (vertical)
            g.fillRoundedRectangle (area.getX() + 2.0f, sliderPos - 7.0f, area.getWidth() - 4.0f, 14.0f, 3.0f);
        else
            g.fillRoundedRectangle (sliderPos - 7.0f, area.getY() + 2.0f, 14.0f, area.getHeight() - 4.0f, 3.0f);

        g.setColour (juce::Colour (panelEdgeColour));
        if (vertical)
            g.drawLine (area.getCentreX(), sliderPos - 4.0f, area.getCentreX(), sliderPos + 4.0f, 1.0f);
        else
            g.drawLine (sliderPos - 4.0f, area.getCentreY(), sliderPos + 4.0f, area.getCentreY(), 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return juce::Font (juce::FontOptions (
            std::min (12.5f, static_cast<float> (buttonHeight) * 0.56f),
            juce::Font::bold));
    }
};
