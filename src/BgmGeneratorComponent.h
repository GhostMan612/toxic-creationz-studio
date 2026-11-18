#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <tracktion_engine/tracktion_engine.h>

#include "RiffLibrary.h"

#include <functional>

namespace te = tracktion::engine;

//==============================================================================
// Toolbar bar for the riff-layering BGM generator: genre / key / energy / bars
// selectors plus Generate and Rescan actions.
//==============================================================================
class BgmGeneratorComponent : public juce::Component
{
public:
    explicit BgmGeneratorComponent (RiffLibrary& targetLibrary);

    void setEdit (te::Edit* newEdit) noexcept { edit = newEdit; }

    std::function<void (int /*clipsCreated*/)> onBgmGenerated;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void triggerGenerateBgm();

    RiffLibrary& library;
    te::Edit* edit = nullptr;

    juce::Label titleLabel;
    juce::ComboBox genreBox;
    juce::ComboBox keyBox;
    juce::ComboBox energyBox;
    juce::ComboBox barsBox;
    juce::TextButton generateButton { "Generate BGM" };
    juce::TextButton rescanButton   { "Rescan Riffs" };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (BgmGeneratorComponent)
};
