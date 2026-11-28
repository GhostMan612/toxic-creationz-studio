#pragma once

// Desktop-only deterministic knowledge matcher. Zero dependencies beyond
// juce_core; used whenever the localhost Ollama brain is unavailable.

#include <juce_core/juce_core.h>

class GhostKnowledge
{
public:
    struct Answer
    {
        juce::String text;
        juce::String sectionId;
        juce::String actionLabel;
        juce::String actionMethod;
        juce::String actionParamsJson;
    };

    struct Section
    {
        const char* id;
        const char* title;
        const char* keywords;
        const char* answer;
        const char* actionLabel;
        const char* actionMethod;
        const char* actionParamsJson;
    };

    static const Section* sections() noexcept;
    static int sectionCount() noexcept;

    static Answer answer (const juce::String& question);
    static juce::StringArray topSectionIds (const juce::String& question, int maxCount = 2);
    static juce::String allSectionsText();
};
