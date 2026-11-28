#pragma once

#include <juce_core/juce_core.h>

namespace ghost
{
struct WalkthroughStep
{
    const char* label;
    const char* method;
    const char* paramsJson;
};

struct Walkthrough
{
    const char* id;
    const char* title;
    const WalkthroughStep* steps;
    int stepCount;
};

inline const WalkthroughStep recordSteps[] = {
    { "Arm track 1", "recording.arm_track", "{\"track_index\":0,\"armed\":true}" },
    { "Enable a 1-bar count-in", "recording.set_count_in", "{\"bars\":1}" },
    { "Start recording", "recording.start", "{}" },
    { "Bounce the take", "recording.export_abc_audition", "{}" }
};

inline const WalkthroughStep beatSteps[] = {
    { "Add a TR-808 module", "rack.add_module", "{\"module_type\":\"tr808\"}" },
    { "Patch it to track 1", "rack.patch", "{}" },
    { "Place a 1-bar pattern", "midi.generate_progression", "{\"bars\":1}" }
};

inline const WalkthroughStep vocalSteps[] = {
    { "Open the VO Booth", "vo.set_script", "{\"text\":\"Welcome to the booth.\"}" },
    { "Arm the vocal track", "recording.arm_track", "{\"track_index\":0,\"armed\":true}" },
    { "Punch and roll the take", "recording.punch_and_roll", "{}" }
};

inline const WalkthroughStep exportSteps[] = {
    { "Export the stereo mixdown", "audio.export_mix", "{}" },
    { "Confirm the file in the Takes directory", "", "" }
};

inline const Walkthrough& walkthroughFor (const juce::String& question)
{
    static const Walkthrough all[] = {
        { "record-first-take", "Record your first take", recordSteps, 4 },
        { "make-808-beat", "Make a beat with the 808 rack", beatSteps, 3 },
        { "mix-vocal-booth", "Mix a vocal with the booth", vocalSteps, 3 },
        { "export-first-mixdown", "Export your first mixdown", exportSteps, 2 }
    };
    const auto q = question.toLowerCase();
    if (q.contains ("record") || q.contains ("take"))   return all[0];
    if (q.contains ("808") || q.contains ("beat"))      return all[1];
    if (q.contains ("vocal") || q.contains ("booth"))   return all[2];
    if (q.contains ("export") || q.contains ("mixdown")) return all[3];
    return all[0];
}

inline const Walkthrough* allWalkthroughs() { return nullptr; }
}
