#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <tracktion_engine/tracktion_engine.h>

#include "CvRackPlugins.h"
#include "HardwareRackPlugins.h"
#include "SidechainCompressorPlugin.h"
#include "VocalChannelPlugin.h"

namespace te = tracktion::engine;

//==============================================================================
// Factory sound preset bank, portable .trkpreset XML serializer/loader, and
// starter multi-sample kit builder for built-in Tracktion & CV plugins.
//==============================================================================
class PluginPresetManager
{
public:
    static juce::File getStudioPresetsRootDir();
    static juce::File getUserPresetsDirectory (const juce::String& pluginType);
    static juce::File getDefaultSamplerKitDirectory();

    static juce::StringArray getFactoryPresetNames (te::Plugin& plugin);
    static bool applyFactoryPreset (te::Plugin& plugin, const juce::String& presetName);

    static juce::Array<juce::File> listUserPresetFiles (te::Plugin& plugin);
    static bool savePresetToFile (te::Plugin& plugin, const juce::File& targetFile);
    static bool loadPresetFromFile (te::Plugin& plugin, const juce::File& sourceFile);

    static bool seedStarterSamplerKit (te::SamplerPlugin& sampler, const juce::File& kitDir = {});
    static bool addSampleFileToSampler (te::SamplerPlugin& sampler,
                                        const juce::File& audioFile,
                                        int rootNote = 60,
                                        int minNote = 0,
                                        int maxNote = 127,
                                        bool openEnded = false);
};
