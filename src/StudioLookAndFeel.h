#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>

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

        setColour (juce::Label::textColourId,                 juce::Colour (0xffe2e8f0));
        setColour (juce::ScrollBar::thumbColourId,            juce::Colour (0xff475569));
    }

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
                         ? juce::Colour (0xff7dd3fc).withAlpha (0.75f)
                         : juce::Colour (0xff334155));
        g.drawRoundedRectangle (bounds, 4.0f, 1.0f);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return juce::Font (juce::FontOptions (
            std::min (12.5f, static_cast<float> (buttonHeight) * 0.56f),
            juce::Font::bold));
    }
};
