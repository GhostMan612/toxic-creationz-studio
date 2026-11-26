#include "AutomationManager.h"

#include <algorithm>
#include <cmath>

int AutomationManager::getInsertIndexBeforeTerminalPlugins (te::AudioTrack& track)
{
    int idx = 0;

    for (auto* p : track.pluginList)
    {
        if (dynamic_cast<te::VolumeAndPanPlugin*> (p) != nullptr
            || dynamic_cast<te::LevelMeterPlugin*> (p) != nullptr)
        {
            break;
        }

        ++idx;
    }

    return idx;
}

te::AutomatableParameter* AutomationManager::resolveTrackParameter (te::AudioTrack& track,
                                                                    const juce::String& paramName)
{
    const auto key = paramName.trim().toLowerCase();

    if (key == "volume" || key == "vol")
        if (auto* vol = track.getVolumePlugin())
            return vol->volParam.get();

    if (key == "pan")
        if (auto* vol = track.getVolumePlugin())
            return vol->panParam.get();

    if (key == "eq_low" || key == "eq_mid" || key == "eq_high")
    {
        for (auto* p : track.pluginList)
        {
            if (auto* eq = dynamic_cast<te::EqualiserPlugin*> (p))
            {
                if (key == "eq_low")  return eq->loGain.get();
                if (key == "eq_mid")  return eq->midGain1.get();
                if (key == "eq_high") return eq->hiGain.get();
            }
        }
    }

    return nullptr;
}

float AutomationManager::convertDomainToParamValue (const juce::String& paramName,
                                                    const te::AutomatableParameter& param,
                                                    float domainValue)
{
    const auto key = paramName.trim().toLowerCase();

    if (key == "volume" || key == "vol")
    {
        const float clampedDb = juce::jlimit (-60.0f, 6.0f, domainValue);
        const float faderPos  = te::decibelsToVolumeFaderPosition (clampedDb);
        return param.valueRange.getRange().clipValue (faderPos);
    }

    return param.valueRange.getRange().clipValue (domainValue);
}

float AutomationManager::convertParamToDomainValue (const juce::String& paramName,
                                                    float paramValue)
{
    const auto key = paramName.trim().toLowerCase();

    if (key == "volume" || key == "vol")
        return te::volumeFaderPositionToDB (paramValue);

    return paramValue;
}

bool AutomationManager::setAutomationPoints (te::Edit& edit,
                                             te::AudioTrack& track,
                                             const juce::String& paramName,
                                             const std::vector<AutomationPointSpec>& points,
                                             bool clearExisting)
{
    auto* param = resolveTrackParameter (track, paramName);

    if (param == nullptr)
        return false;

    auto& um = edit.getUndoManager();
    um.beginNewTransaction ("Edit Track Automation");

    auto& curve = param->getCurve();

    if (clearExisting)
        curve.clear (&um);

    for (const auto& pt : points)
    {
        const auto posTime = edit.tempoSequence.toTime (
            tracktion::BeatPosition::fromBeats (std::max (0.0, pt.beat)));
        const float rawVal = convertDomainToParamValue (paramName, *param, pt.value);
        const float curvature = juce::jlimit (-1.0f, 1.0f, pt.curve);
        curve.addPoint (tracktion::EditPosition (posTime), rawVal, curvature, &um);
    }

    return true;
}

std::vector<AutomationPointSpec> AutomationManager::getAutomationPoints (te::Edit& edit,
                                                                         te::AudioTrack& track,
                                                                         const juce::String& paramName)
{
    std::vector<AutomationPointSpec> out;
    auto* param = resolveTrackParameter (track, paramName);

    if (param == nullptr)
        return out;

    const auto& curve = param->getCurve();
    const int n = curve.getNumPoints();
    out.reserve (static_cast<size_t> (n));

    for (int i = 0; i < n; ++i)
    {
        AutomationPointSpec spec;
        const auto ptTime = tracktion::toTime (curve.getPointPosition (i), edit.tempoSequence);
        spec.beat  = edit.tempoSequence.toBeats (ptTime).inBeats();
        spec.value = convertParamToDomainValue (paramName, curve.getPointValue (i));
        spec.curve = curve.getPointCurve (i);
        out.push_back (spec);
    }

    std::sort (out.begin(), out.end(),
               [] (const AutomationPointSpec& a, const AutomationPointSpec& b)
               {
                   return a.beat < b.beat;
               });

    return out;
}

bool AutomationManager::removeAutomationPointAtIndex (te::Edit& edit,
                                                      te::AudioTrack& track,
                                                      const juce::String& paramName,
                                                      int pointIndex)
{
    auto* param = resolveTrackParameter (track, paramName);

    if (param == nullptr)
        return false;

    auto& curve = param->getCurve();

    if (! juce::isPositiveAndBelow (pointIndex, curve.getNumPoints()))
        return false;

    auto& um = edit.getUndoManager();
    um.beginNewTransaction ("Remove Automation Point");
    curve.removePoint (pointIndex, &um);
    return true;
}

bool AutomationManager::moveAutomationPointAtIndex (te::Edit& edit,
                                                    te::AudioTrack& track,
                                                    const juce::String& paramName,
                                                    int pointIndex,
                                                    double newBeat,
                                                    float newDomainValue,
                                                    int* outNewIndex)
{
    auto* param = resolveTrackParameter (track, paramName);

    if (param == nullptr)
        return false;

    auto& curve = param->getCurve();

    if (! juce::isPositiveAndBelow (pointIndex, curve.getNumPoints()))
        return false;

    const auto posTime = edit.tempoSequence.toTime (
        tracktion::BeatPosition::fromBeats (std::max (0.0, newBeat)));
    const float rawVal = convertDomainToParamValue (paramName, *param, newDomainValue);

    const int newIdx = curve.movePoint (pointIndex,
                                        tracktion::EditPosition (posTime),
                                        rawVal,
                                        param->valueRange.getRange(),
                                        false,
                                        &edit.getUndoManager());

    if (outNewIndex != nullptr)
        *outNewIndex = newIdx;

    return true;
}

FxSendResult AutomationManager::configureFxSend (te::Edit& edit,
                                                 te::AudioTrack& sourceTrack,
                                                 int busIndex,
                                                 float sendGainDb,
                                                 const juce::String& effectType,
                                                 float wetLevel)
{
    FxSendResult result;
    const int clampedBus   = juce::jlimit (0, 7, busIndex);
    const float clampedDb  = juce::jlimit (-60.0f, 6.0f, sendGainDb);
    const float clampedWet = juce::jlimit (0.0f, 1.0f, wetLevel);

    auto& um = edit.getUndoManager();
    um.beginNewTransaction ("Configure FX Send Bus");

    // 1. Find or create an FX Return track hosting AuxReturnPlugin on clampedBus.
    te::AudioTrack* returnTrack = nullptr;
    int returnTrackIdx = -1;
    auto tracks = te::getAudioTracks (edit);

    for (int i = 0; i < tracks.size(); ++i)
    {
        for (auto* p : tracks[i]->pluginList)
        {
            if (auto* ret = dynamic_cast<te::AuxReturnPlugin*> (p))
            {
                if (ret->busNumber.get() == clampedBus)
                {
                    returnTrack = tracks[i];
                    returnTrackIdx = i;
                    break;
                }
            }
        }

        if (returnTrack != nullptr)
            break;
    }

    if (returnTrack == nullptr)
    {
        edit.ensureNumberOfAudioTracks (tracks.size() + 1);
        tracks = te::getAudioTracks (edit);
        returnTrack = tracks.getLast();
        returnTrackIdx = tracks.size() - 1;
        returnTrack->setName ("FX Return (Reverb/Delay)");

        if (auto retPlugin = edit.getPluginCache().createNewPlugin (te::AuxReturnPlugin::xmlTypeName, {}))
        {
            if (auto* ret = dynamic_cast<te::AuxReturnPlugin*> (retPlugin.get()))
                ret->busNumber = clampedBus;

            returnTrack->pluginList.insertPlugin (
                retPlugin, getInsertIndexBeforeTerminalPlugins (*returnTrack), nullptr);
        }
    }

    // 2. Ensure built-in Reverb or Delay exists on the FX Return track.
    const auto fxLower = effectType.trim().toLowerCase();

    if (fxLower == "delay")
    {
        te::DelayPlugin* delay = nullptr;

        for (auto* p : returnTrack->pluginList)
            if (auto* d = dynamic_cast<te::DelayPlugin*> (p))
                delay = d;

        if (delay == nullptr)
        {
            if (auto dp = edit.getPluginCache().createNewPlugin (te::DelayPlugin::xmlTypeName, {}))
            {
                delay = dynamic_cast<te::DelayPlugin*> (dp.get());
                returnTrack->pluginList.insertPlugin (
                    dp, getInsertIndexBeforeTerminalPlugins (*returnTrack), nullptr);
            }
        }

        if (delay != nullptr)
        {
            delay->mixValue = clampedWet;
            if (delay->mixProportion != nullptr)
                delay->mixProportion->updateFromAttachedValue();
        }
    }
    else
    {
        te::ReverbPlugin* reverb = nullptr;

        for (auto* p : returnTrack->pluginList)
            if (auto* r = dynamic_cast<te::ReverbPlugin*> (p))
                reverb = r;

        if (reverb == nullptr)
        {
            if (auto rp = edit.getPluginCache().createNewPlugin (te::ReverbPlugin::xmlTypeName, {}))
            {
                reverb = dynamic_cast<te::ReverbPlugin*> (rp.get());
                returnTrack->pluginList.insertPlugin (
                    rp, getInsertIndexBeforeTerminalPlugins (*returnTrack), nullptr);
            }
        }

        if (reverb != nullptr)
        {
            reverb->wetValue = clampedWet;
            if (reverb->wetParam != nullptr)
                reverb->wetParam->updateFromAttachedValue();
        }
    }

    // 3. Ensure sourceTrack has an AuxSendPlugin routed to clampedBus and update gain synchronously.
    te::AuxSendPlugin* send = nullptr;

    for (auto* p : sourceTrack.pluginList)
    {
        if (auto* s = dynamic_cast<te::AuxSendPlugin*> (p))
        {
            if (s->busNumber.get() == clampedBus)
            {
                send = s;
                break;
            }
        }
    }

    if (send == nullptr)
    {
        if (auto sp = edit.getPluginCache().createNewPlugin (te::AuxSendPlugin::xmlTypeName, {}))
        {
            send = dynamic_cast<te::AuxSendPlugin*> (sp.get());

            if (send != nullptr)
                send->busNumber = clampedBus;

            sourceTrack.pluginList.insertPlugin (
                sp, getInsertIndexBeforeTerminalPlugins (sourceTrack), nullptr);
        }
    }

    if (send == nullptr)
        return result;

    send->gainLevel = te::decibelsToVolumeFaderPosition (clampedDb);

    if (send->gain != nullptr)
        send->gain->updateFromAttachedValue();

    result.success = true;
    result.returnTrackIndex = returnTrackIdx;
    result.busIndex = clampedBus;
    result.sendDb = send->getGainDb();
    result.wetLevel = clampedWet;
    return result;
}
