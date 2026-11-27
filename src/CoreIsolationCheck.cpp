#include "CoreIsolationCheck.h"

#include "PlatformConfig.h"

// Platform-neutral headers only. No StudioPluginWindow / MainComponent /
// MixerComponent / ModularRackComponent / ArrangementView / PianoRollComponent.
#include "AudioToMidiTranscriber.h"
#include "HardwareRackPlugins.h"
#include "SidechainCompressorPlugin.h"
#include "VocalChannelPlugin.h"

namespace tcs
{
namespace core
{
    CoreTypeIds getCoreTypeIds()
    {
        CoreTypeIds ids;
        ids.tr808      = Tr808DrumPlugin::xmlTypeName;
        ids.sampler    = VintageSamplerPlugin::xmlTypeName;
        ids.tape       = AnalogTapePlugin::xmlTypeName;
        ids.sidechain  = SidechainCompressorPlugin::xmlTypeName;
        ids.vocal      = VocalChannelPlugin::xmlTypeName;
        ids.transcriber = "AudioToMidiTranscriber";
        return ids;
    }
}
}
