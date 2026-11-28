#include "GhostKnowledge.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace
{
// Keep this table in sync with docs/GHOST_KNOWLEDGE.md. It is compiled into the
// app so a standalone EXE still has the full fallback guide.
const GhostKnowledge::Section knowledgeSections[] = {
    { "record-arming", "Record your first take",
      "record arm countin take rec track microphone",
      "Arm a track, optionally enable Count-In, press Rec, play, then Stop. Your take lands as a clip on the armed track.",
      "Arm track 1 for recording", "recording.arm_track", "{\"track_index\":0,\"armed\":true}" },
    { "piano-roll-step-rec", "Piano roll and Step Rec",
      "piano roll step rec notes midi clip grid keyboard",
      "Open Keys to show the piano roll, select a MIDI clip, click the grid to add notes, or tap Step Rec to enter notes in time.",
      "Create a 4-bar MIDI clip", "track.create", "{\"track_name\":\"MIDI Idea\",\"kind\":\"midi\",\"bars\":4}" },
    { "mixer-eq-meters", "Mixer, EQ and meters",
      "mixer fader pan eq meter gain volume balance",
      "Open Mixer, drag the fader for level and the pan knob for placement, then shape tone with the three-band EQ. Watch the meter.",
      "Balance the mix to -14 LUFS", "mixer.auto_balance_lufs", "{\"target_lufs\":-14.0}" },
    { "rack-patching-hardware", "Rack patching and hardware modules",
      "rack patch cable module 808 sampler tape sidechain cv pin",
      "Open Rack, add a module with +, drag between output and input pins to patch, and right-click a cable to disconnect it.",
      "Add a TR-808 module", "rack.add_module", "{\"module_type\":\"tr808\"}" },
    { "bgm-solver", "BGM solver and song forms",
      "bgm generate beat genre key scale seed song form bars",
      "In the BGM Gen bar choose a genre and key, then scale, bars and a seed. Hit Randomize for variety and Generate BGM to lay out stems.",
      "Generate a 16-bar BGM", "bgm.generate", "{\"genre\":\"Cyberpunk\",\"key\":\"Am\",\"bars\":16}" },
    { "waveform-bounce", "Waveform editor and bounce-to-take",
      "wave edit bounce normalize reverse fade take clip",
      "Select an audio clip, open Wave Edit, pick a range and apply normalize, reverse or fade. The result is a take; the original stays intact.",
      "Normalize the selected clip", "audio.process_clip", "{\"operation\":\"normalize\"}" },
    { "mixdown-export", "Mixdown export",
      "export wav mixdown master file render loop",
      "Set the export range or use the default edit length, then click Export WAV. The file is written as 44.1 kHz / 16-bit PCM.",
      "Export a stereo mixdown", "audio.export_mix", "{}" },
    { "transcription", "Audio-to-MIDI transcription",
      "transcribe pitch midi yin basic note audio to midi quantize",
      "Select an audio clip and run Wave-to-MIDI. The YIN or basic-pitch backend writes a MIDI clip you can quantize to a key and scale.",
      "Transcribe the selected clip", "audio.transcribe_to_midi", "{}" },
    { "voice-acting-booth", "Voice Acting booth",
      "voice vo booth script teleprompter adr punch roll character fx",
      "Open VO Booth, load a practice script, arm the vocal track and read the teleprompter. Use Punch and Roll to replace a take.",
      "Load a VO practice script", "vo.set_script", "{\"text\":\"Welcome to the booth.\"}" },
    { "cross-platform", "Cross-platform notes",
      "windows mac ubuntu linux core isolation oboe android vst3 build",
      "tcs_core stays platform-neutral; the desktop UI lives in AIDAW. Windows uses MSVC, macOS and Ubuntu build tcs_core_isolation, and Oboe defines apply to the core only.",
      "", "", "" },
    { "game-audio-forge", "Game Audio Forge",
      "game sfx forge godot manifest adaptive stems bark loop",
      "In Game Forge, forge seeded SFX variations, export a seamless loop or adaptive stems, then export a Godot audio pack with audio_manifest.json.",
      "Forge six UI clicks", "game.forge_sfx_batch", "{\"category\":\"ui_click\",\"count\":6}" },
    { "live-soundboard", "Pathfinder God live soundboard",
      "rpg soundboard biome mood pad gm voice encounter dice dragon",
      "In RPG Board pick a biome and mood, tap a pad to fire a procedural SFX, assign custom WAVs to the six user slots, and swap the GM voice preset.",
      "Reveal the boss encounter", "rpg.apply_encounter_cue", "{\"event\":\"boss_reveal\"}" }
};
}

const GhostKnowledge::Section* GhostKnowledge::sections() noexcept { return knowledgeSections; }
int GhostKnowledge::sectionCount() noexcept { return static_cast<int> (sizeof (knowledgeSections) / sizeof (knowledgeSections[0])); }

static int scoreSection (const GhostKnowledge::Section& section, const juce::String& question)
{
    const auto words = juce::StringArray::fromTokens (question.toLowerCase(), " ,.?!:;", "");
    int score = 0;
    for (const auto& word : words)
    {
        if (word.length() < 3)
            continue;
        if (juce::String (section.keywords).contains (word))
            score += 3;
        if (juce::String (section.title).contains (word))
            score += 2;
    }
    return score;
}

GhostKnowledge::Answer GhostKnowledge::answer (const juce::String& question)
{
    Answer result;
    int bestScore = -1;
    for (int i = 0; i < sectionCount(); ++i)
    {
        const auto& section = knowledgeSections[static_cast<size_t> (i)];
        const int score = scoreSection (section, question);
        if (score > bestScore)
        {
            bestScore = score;
            result.sectionId = section.id;
            result.text = juce::String (section.answer);
            result.actionLabel = section.actionLabel;
            result.actionMethod = section.actionMethod;
            result.actionParamsJson = section.actionParamsJson;
        }
    }
    if (bestScore <= 0)
    {
        result.sectionId = "record-arming";
        result.text = "Boo. I only know the built-in guide. Try asking about recording, the mixer, rack patching, BGM, voice booth, game audio or the RPG board.";
        result.actionLabel = "";
        result.actionMethod = "";
        result.actionParamsJson = "";
    }
    return result;
}

juce::StringArray GhostKnowledge::topSectionIds (const juce::String& question, int maxCount)
{
    std::vector<std::pair<int, juce::String>> ranked;
    for (int i = 0; i < sectionCount(); ++i)
    {
        const auto& section = knowledgeSections[static_cast<size_t> (i)];
        ranked.push_back ({ scoreSection (section, question), section.id });
    }
    std::stable_sort (ranked.begin(), ranked.end(), [] (const auto& lhs, const auto& rhs)
    {
        return lhs.first > rhs.first;
    });

    juce::StringArray out;
    for (const auto& item : ranked)
    {
        if (out.size() >= juce::jmax (1, maxCount))
            break;
        if (item.first > 0)
            out.add (item.second);
    }
    return out;
}

juce::String GhostKnowledge::allSectionsText()
{
    juce::String out;
    for (int i = 0; i < sectionCount(); ++i)
    {
        const auto& section = knowledgeSections[static_cast<size_t> (i)];
        out << section.id << ": " << section.title << " — " << section.answer << "\n";
    }
    return out;
}
