#pragma once

#include <juce_core/juce_core.h>
#include <tracktion_engine/tracktion_engine.h>

#include <vector>

namespace te = tracktion::engine;

struct AutomationPointSpec
{
    double beat = 0.0;
    float value = 0.0f;  // Domain value (dB for volume, -1..1 for pan, dB for EQ)
    float curve = 0.0f;  // Tracktion curvature (-1..1, 0 = linear)
};

struct FxSendResult
{
    bool success = false;
    int returnTrackIndex = -1;
    int busIndex = 0;
    float sendDb = 0.0f;
    float wetLevel = 0.35f;
};

class AutomationManager
{
public:
    static te::AutomatableParameter* resolveTrackParameter (te::AudioTrack& track,
                                                            const juce::String& paramName);

    static float convertDomainToParamValue (const juce::String& paramName,
                                            const te::AutomatableParameter& param,
                                            float domainValue);

    static float convertParamToDomainValue (const juce::String& paramName,
                                            float paramValue);

    static bool setAutomationPoints (te::Edit& edit,
                                     te::AudioTrack& track,
                                     const juce::String& paramName,
                                     const std::vector<AutomationPointSpec>& points,
                                     bool clearExisting = true);

    static std::vector<AutomationPointSpec> getAutomationPoints (te::Edit& edit,
                                                                 te::AudioTrack& track,
                                                                 const juce::String& paramName);

    static bool removeAutomationPointAtIndex (te::Edit& edit,
                                              te::AudioTrack& track,
                                              const juce::String& paramName,
                                              int pointIndex);

    static bool moveAutomationPointAtIndex (te::Edit& edit,
                                            te::AudioTrack& track,
                                            const juce::String& paramName,
                                            int pointIndex,
                                            double newBeat,
                                            float newDomainValue,
                                            int* outNewIndex = nullptr);

    static FxSendResult configureFxSend (te::Edit& edit,
                                         te::AudioTrack& sourceTrack,
                                         int busIndex,
                                         float sendGainDb,
                                         const juce::String& effectType = "reverb",
                                         float wetLevel = 0.35f);

    static int getInsertIndexBeforeTerminalPlugins (te::AudioTrack& track);
};
