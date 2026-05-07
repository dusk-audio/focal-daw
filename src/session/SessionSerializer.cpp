#include "SessionSerializer.h"

namespace focal
{
namespace
{
constexpr int kFormatVersion = 1;

juce::String colourToHex (juce::Colour c)
{
    return juce::String::toHexString ((int) c.getARGB()).paddedLeft ('0', 8);
}

juce::Colour hexToColour (const juce::String& s, juce::Colour fallback)
{
    if (s.isEmpty()) return fallback;
    auto v = (juce::uint32) s.getHexValue64();
    return juce::Colour (v);
}

juce::DynamicObject::Ptr trackToObject (const Track& t)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("name",   t.name);
    obj->setProperty ("colour", colourToHex (t.colour));

    // Plugin-slot persistence. Empty strings (no plugin loaded) are written
    // verbatim - round-trip restoreFromSavedState treats empty as "no
    // plugin", which is the correct steady state for unused slots.
    if (t.pluginDescriptionXml.isNotEmpty())
        obj->setProperty ("plugin_desc_xml", t.pluginDescriptionXml);
    if (t.pluginStateBase64.isNotEmpty())
        obj->setProperty ("plugin_state",    t.pluginStateBase64);

    obj->setProperty ("fader_db",       t.strip.faderDb.load());
    obj->setProperty ("pan",            t.strip.pan.load());
    obj->setProperty ("mute",           t.strip.mute.load());
    obj->setProperty ("solo",           t.strip.solo.load());
    obj->setProperty ("phase_invert",   t.strip.phaseInvert.load());
    obj->setProperty ("input_monitor",  t.inputMonitor.load());
    obj->setProperty ("print_effects",  t.printEffects.load());
    obj->setProperty ("input_source",   t.inputSource.load());
    obj->setProperty ("input_source_r", t.inputSourceR.load());
    obj->setProperty ("midi_input_idx", t.midiInputIndex.load());
    obj->setProperty ("track_mode",     t.mode.load());

    juce::Array<juce::var> buses;
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
        buses.add (t.strip.busAssign[(size_t) i].load());
    obj->setProperty ("bus_assign", buses);

    // Aux sends (continuous send level + pre/post-fader tap) - distinct
    // from busAssign which is the post-fader on/off routing toggle.
    juce::Array<juce::var> auxLevels, auxPrePost;
    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        auxLevels .add ((double) t.strip.auxSendDb[(size_t) i].load());
        auxPrePost.add (         t.strip.auxSendPreFader[(size_t) i].load());
    }
    obj->setProperty ("aux_send_db",        auxLevels);
    obj->setProperty ("aux_send_pre_fader", auxPrePost);

    auto* hpf = new juce::DynamicObject();
    hpf->setProperty ("enabled", t.strip.hpfEnabled.load());
    hpf->setProperty ("freq",    t.strip.hpfFreq.load());
    obj->setProperty ("hpf", juce::var (hpf));

    auto* eq = new juce::DynamicObject();
    eq->setProperty ("type", t.strip.eqBlackMode.load() ? "black" : "brown");
    auto bandObj = [] (float gain, float freq, float q = -1.0f)
    {
        auto* b = new juce::DynamicObject();
        b->setProperty ("gain", gain);
        b->setProperty ("freq", freq);
        if (q >= 0.0f) b->setProperty ("q", q);
        return juce::var (b);
    };
    eq->setProperty ("lf", bandObj (t.strip.lfGainDb.load(), t.strip.lfFreq.load()));
    eq->setProperty ("lm", bandObj (t.strip.lmGainDb.load(), t.strip.lmFreq.load(), t.strip.lmQ.load()));
    eq->setProperty ("hm", bandObj (t.strip.hmGainDb.load(), t.strip.hmFreq.load(), t.strip.hmQ.load()));
    eq->setProperty ("hf", bandObj (t.strip.hfGainDb.load(), t.strip.hfFreq.load()));
    obj->setProperty ("eq", juce::var (eq));

    auto* comp = new juce::DynamicObject();
    comp->setProperty ("enabled",  t.strip.compEnabled.load());
    comp->setProperty ("mode",     t.strip.compMode.load());
    comp->setProperty ("threshold_db", t.strip.compThresholdDb.load());  // legacy meter-strip drag
    // Per-mode parameters - UniversalCompressor's native shape.
    comp->setProperty ("opto_peak_red", t.strip.compOptoPeakRed.load());
    comp->setProperty ("opto_gain",     t.strip.compOptoGain.load());
    comp->setProperty ("opto_limit",    t.strip.compOptoLimit.load());
    comp->setProperty ("fet_input",     t.strip.compFetInput.load());
    comp->setProperty ("fet_output",    t.strip.compFetOutput.load());
    comp->setProperty ("fet_attack",    t.strip.compFetAttack.load());
    comp->setProperty ("fet_release",   t.strip.compFetRelease.load());
    comp->setProperty ("fet_ratio",     t.strip.compFetRatio.load());
    comp->setProperty ("vca_thresh_db", t.strip.compVcaThreshDb.load());
    comp->setProperty ("vca_ratio",     t.strip.compVcaRatio.load());
    comp->setProperty ("vca_attack",    t.strip.compVcaAttack.load());
    comp->setProperty ("vca_release",   t.strip.compVcaRelease.load());
    comp->setProperty ("vca_output",    t.strip.compVcaOutput.load());
    obj->setProperty ("comp", juce::var (comp));

    juce::Array<juce::var> regions;
    for (auto& r : t.regions)
    {
        auto* rObj = new juce::DynamicObject();
        rObj->setProperty ("file",            r.file.getFullPathName());
        rObj->setProperty ("timeline_start",  (juce::int64) r.timelineStart);
        rObj->setProperty ("length",          (juce::int64) r.lengthInSamples);
        rObj->setProperty ("source_offset",   (juce::int64) r.sourceOffset);

        // Take history. Empty array on the common case (no overdubs); only
        // serialised when at least one prior take has been captured to keep
        // session.json compact.
        if (! r.previousTakes.empty())
        {
            juce::Array<juce::var> prior;
            for (auto& take : r.previousTakes)
            {
                auto* tObj = new juce::DynamicObject();
                tObj->setProperty ("file",          take.file.getFullPathName());
                tObj->setProperty ("source_offset", (juce::int64) take.sourceOffset);
                tObj->setProperty ("length",        (juce::int64) take.lengthInSamples);
                prior.add (juce::var (tObj));
            }
            rObj->setProperty ("previous_takes", prior);
        }

        regions.add (juce::var (rObj));
    }
    obj->setProperty ("regions", regions);

    return obj;
}

juce::DynamicObject::Ptr busToObject (const Bus& a)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("name",     a.name);
    obj->setProperty ("colour",   colourToHex (a.colour));
    obj->setProperty ("fader_db", a.strip.faderDb.load());
    obj->setProperty ("pan",      a.strip.pan.load());
    obj->setProperty ("mute",     a.strip.mute.load());
    obj->setProperty ("solo",     a.strip.solo.load());

    obj->setProperty ("eq_enabled",   a.strip.eqEnabled.load());
    obj->setProperty ("eq_lf_db",     a.strip.eqLfGainDb.load());
    obj->setProperty ("eq_mid_db",    a.strip.eqMidGainDb.load());
    obj->setProperty ("eq_hf_db",     a.strip.eqHfGainDb.load());

    obj->setProperty ("comp_enabled",     a.strip.compEnabled.load());
    obj->setProperty ("comp_thresh_db",   a.strip.compThreshDb.load());
    obj->setProperty ("comp_ratio",       a.strip.compRatio.load());
    obj->setProperty ("comp_attack_ms",   a.strip.compAttackMs.load());
    obj->setProperty ("comp_release_ms",  a.strip.compReleaseMs.load());
    obj->setProperty ("comp_makeup_db",   a.strip.compMakeupDb.load());

    return obj;
}

void restoreTrack (Track& t, const juce::var& v)
{
    if (! v.isObject()) return;
    if (auto s = v["name"].toString();           s.isNotEmpty()) t.name = s;
    if (auto s = v["colour"].toString();         s.isNotEmpty()) t.colour = hexToColour (s, t.colour);

    // Plugin slot - strings remain empty when the property is absent (older
    // sessions or unused slots). AudioEngine::consumePluginStateAfterLoad
    // reads these back and asks each PluginSlot to reinstantiate.
    t.pluginDescriptionXml = v["plugin_desc_xml"].toString();
    t.pluginStateBase64    = v["plugin_state"]   .toString();

    auto setFloat = [&v] (std::atomic<float>& a, const char* key)
    {
        if (v.hasProperty (key)) a.store ((float) (double) v[key], std::memory_order_relaxed);
    };
    auto setBool = [&v] (std::atomic<bool>& a, const char* key)
    {
        if (v.hasProperty (key)) a.store ((bool) v[key], std::memory_order_relaxed);
    };
    auto setInt = [&v] (std::atomic<int>& a, const char* key)
    {
        if (v.hasProperty (key)) a.store ((int) v[key], std::memory_order_relaxed);
    };

    setFloat (t.strip.faderDb,      "fader_db");
    setFloat (t.strip.pan,          "pan");
    setBool  (t.strip.mute,         "mute");
    setBool  (t.strip.solo,         "solo");
    setBool  (t.strip.phaseInvert,  "phase_invert");
    setBool  (t.inputMonitor,       "input_monitor");
    setBool  (t.printEffects,       "print_effects");
    setInt   (t.inputSource,        "input_source");
    setInt   (t.inputSourceR,       "input_source_r");
    setInt   (t.midiInputIndex,     "midi_input_idx");
    setInt   (t.mode,               "track_mode");

    if (auto buses = v["bus_assign"]; buses.isArray())
    {
        const int n = juce::jmin (ChannelStripParams::kNumBuses, buses.size());
        for (int i = 0; i < n; ++i)
            t.strip.busAssign[(size_t) i].store ((bool) buses[i], std::memory_order_relaxed);
    }

    if (auto auxLevels = v["aux_send_db"]; auxLevels.isArray())
    {
        const int n = juce::jmin (ChannelStripParams::kNumAuxSends, auxLevels.size());
        for (int i = 0; i < n; ++i)
            t.strip.auxSendDb[(size_t) i].store ((float) (double) auxLevels[i],
                                                   std::memory_order_relaxed);
    }
    if (auto auxPrePost = v["aux_send_pre_fader"]; auxPrePost.isArray())
    {
        const int n = juce::jmin (ChannelStripParams::kNumAuxSends, auxPrePost.size());
        for (int i = 0; i < n; ++i)
            t.strip.auxSendPreFader[(size_t) i].store ((bool) auxPrePost[i],
                                                          std::memory_order_relaxed);
    }

    if (auto hpf = v["hpf"]; hpf.isObject())
    {
        if (hpf.hasProperty ("enabled")) t.strip.hpfEnabled.store ((bool) hpf["enabled"]);
        if (hpf.hasProperty ("freq"))    t.strip.hpfFreq.store ((float) (double) hpf["freq"]);
    }

    if (auto eq = v["eq"]; eq.isObject())
    {
        if (auto type = eq["type"].toString(); type.isNotEmpty())
            t.strip.eqBlackMode.store (type == "black");

        auto restoreBand = [&eq] (const char* key, std::atomic<float>* gain,
                                   std::atomic<float>* freq, std::atomic<float>* q)
        {
            auto b = eq[key];
            if (! b.isObject()) return;
            if (gain && b.hasProperty ("gain")) gain->store ((float) (double) b["gain"]);
            if (freq && b.hasProperty ("freq")) freq->store ((float) (double) b["freq"]);
            if (q    && b.hasProperty ("q"))    q->store    ((float) (double) b["q"]);
        };
        restoreBand ("lf", &t.strip.lfGainDb, &t.strip.lfFreq, nullptr);
        restoreBand ("lm", &t.strip.lmGainDb, &t.strip.lmFreq, &t.strip.lmQ);
        restoreBand ("hm", &t.strip.hmGainDb, &t.strip.hmFreq, &t.strip.hmQ);
        restoreBand ("hf", &t.strip.hfGainDb, &t.strip.hfFreq, nullptr);
    }

    if (auto comp = v["comp"]; comp.isObject())
    {
        auto loadF = [&] (const char* key, std::atomic<float>& dst)
        {
            if (comp.hasProperty (key)) dst.store ((float) (double) comp[key]);
        };
        auto loadI = [&] (const char* key, std::atomic<int>& dst)
        {
            if (comp.hasProperty (key)) dst.store ((int) comp[key]);
        };
        auto loadB = [&] (const char* key, std::atomic<bool>& dst)
        {
            if (comp.hasProperty (key)) dst.store ((bool) comp[key]);
        };
        loadB ("enabled", t.strip.compEnabled);
        loadI ("mode",    t.strip.compMode);
        loadF ("threshold_db", t.strip.compThresholdDb);
        loadF ("opto_peak_red", t.strip.compOptoPeakRed);
        loadF ("opto_gain",     t.strip.compOptoGain);
        loadB ("opto_limit",    t.strip.compOptoLimit);
        loadF ("fet_input",     t.strip.compFetInput);
        loadF ("fet_output",    t.strip.compFetOutput);
        loadF ("fet_attack",    t.strip.compFetAttack);
        loadF ("fet_release",   t.strip.compFetRelease);
        loadI ("fet_ratio",     t.strip.compFetRatio);
        loadF ("vca_thresh_db", t.strip.compVcaThreshDb);
        loadF ("vca_ratio",     t.strip.compVcaRatio);
        loadF ("vca_attack",    t.strip.compVcaAttack);
        loadF ("vca_release",   t.strip.compVcaRelease);
        loadF ("vca_output",    t.strip.compVcaOutput);
    }

    t.regions.clear();
    if (auto regions = v["regions"]; regions.isArray())
    {
        for (int i = 0; i < regions.size(); ++i)
        {
            auto rv = regions[i];
            if (! rv.isObject()) continue;
            AudioRegion r;
            r.file            = juce::File (rv["file"].toString());
            r.timelineStart   = (juce::int64) rv["timeline_start"];
            r.lengthInSamples = (juce::int64) rv["length"];
            r.sourceOffset    = (juce::int64) rv["source_offset"];

            if (auto prior = rv["previous_takes"]; prior.isArray())
            {
                for (int k = 0; k < prior.size(); ++k)
                {
                    auto tv = prior[k];
                    if (! tv.isObject()) continue;
                    TakeRef take;
                    take.file            = juce::File (tv["file"].toString());
                    take.sourceOffset    = (juce::int64) tv["source_offset"];
                    take.lengthInSamples = (juce::int64) tv["length"];
                    r.previousTakes.push_back (std::move (take));
                }
            }

            t.regions.push_back (std::move (r));
        }
    }
}

void restoreBus (Bus& a, const juce::var& v)
{
    if (! v.isObject()) return;
    if (auto s = v["name"].toString();   s.isNotEmpty()) a.name = s;
    if (auto s = v["colour"].toString(); s.isNotEmpty()) a.colour = hexToColour (s, a.colour);
    if (v.hasProperty ("fader_db"))                       a.strip.faderDb.store ((float) (double) v["fader_db"]);
    if (v.hasProperty ("pan"))                            a.strip.pan.store     ((float) (double) v["pan"]);
    if (v.hasProperty ("mute"))                           a.strip.mute.store ((bool) v["mute"]);
    if (v.hasProperty ("solo"))                           a.strip.solo.store ((bool) v["solo"]);

    if (v.hasProperty ("eq_enabled"))   a.strip.eqEnabled  .store ((bool)  v["eq_enabled"]);
    if (v.hasProperty ("eq_lf_db"))     a.strip.eqLfGainDb .store ((float) (double) v["eq_lf_db"]);
    if (v.hasProperty ("eq_mid_db"))    a.strip.eqMidGainDb.store ((float) (double) v["eq_mid_db"]);
    if (v.hasProperty ("eq_hf_db"))     a.strip.eqHfGainDb .store ((float) (double) v["eq_hf_db"]);

    if (v.hasProperty ("comp_enabled"))    a.strip.compEnabled  .store ((bool)  v["comp_enabled"]);
    if (v.hasProperty ("comp_thresh_db"))  a.strip.compThreshDb .store ((float) (double) v["comp_thresh_db"]);
    if (v.hasProperty ("comp_ratio"))      a.strip.compRatio    .store ((float) (double) v["comp_ratio"]);
    if (v.hasProperty ("comp_attack_ms"))  a.strip.compAttackMs .store ((float) (double) v["comp_attack_ms"]);
    if (v.hasProperty ("comp_release_ms")) a.strip.compReleaseMs.store ((float) (double) v["comp_release_ms"]);
    if (v.hasProperty ("comp_makeup_db"))  a.strip.compMakeupDb .store ((float) (double) v["comp_makeup_db"]);
}
} // namespace

bool SessionSerializer::save (const Session& s, const juce::File& target)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("version", kFormatVersion);

    juce::Array<juce::var> tracks;
    for (int i = 0; i < Session::kNumTracks; ++i)
        tracks.add (juce::var (trackToObject (s.track (i)).get()));
    root->setProperty ("tracks", tracks);

    juce::Array<juce::var> busesArr;
    for (int i = 0; i < Session::kNumBuses; ++i)
        busesArr.add (juce::var (busToObject (s.bus (i)).get()));
    root->setProperty ("buses", busesArr);

    juce::Array<juce::var> auxLanesArr;
    for (int i = 0; i < Session::kNumAuxLanes; ++i)
    {
        const auto& lane = s.auxLane (i);
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("name",   lane.name);
        obj->setProperty ("colour", colourToHex (lane.colour));
        obj->setProperty ("return_level_db", lane.params.returnLevelDb.load());
        obj->setProperty ("mute",            lane.params.mute.load());
        // Per-slot plugin state. Empty strings serialise as empty - same
        // pattern as Track.
        juce::Array<juce::var> slots;
        for (int p = 0; p < AuxLaneParams::kMaxLanePlugins; ++p)
        {
            auto* slot = new juce::DynamicObject();
            slot->setProperty ("plugin_desc_xml", lane.pluginDescriptionXml[(size_t) p]);
            slot->setProperty ("plugin_state",    lane.pluginStateBase64[(size_t) p]);
            slots.add (juce::var (slot));
        }
        obj->setProperty ("plugin_slots", slots);
        auxLanesArr.add (juce::var (obj));
    }
    root->setProperty ("aux_lanes", auxLanesArr);

    juce::Array<juce::var> markersArr;
    for (const auto& m : s.getMarkers())
    {
        auto* obj = new juce::DynamicObject();
        obj->setProperty ("name",   m.name);
        obj->setProperty ("time",   (juce::int64) m.timelineSamples);
        obj->setProperty ("colour", colourToHex (m.colour));
        markersArr.add (juce::var (obj));
    }
    root->setProperty ("markers", markersArr);

    auto* master = new juce::DynamicObject();
    master->setProperty ("fader_db",     s.master().faderDb.load());
    master->setProperty ("tape_enabled", s.master().tapeEnabled.load());
    master->setProperty ("tape_hq",      s.master().tapeHQ.load());
    root->setProperty ("master", juce::var (master));

    // Mastering chain - separate from the master strip so its EQ/comp/limiter
    // settings can diverge from the in-mix master DSP.
    auto* mast = new juce::DynamicObject();
    mast->setProperty ("source_file",       s.mastering().sourceFile.getFullPathName());
    mast->setProperty ("eq_enabled",        s.mastering().eqEnabled.load());
    mast->setProperty ("eq_lf_boost",       s.mastering().eqLfBoost.load());
    mast->setProperty ("eq_hf_boost",       s.mastering().eqHfBoost.load());
    mast->setProperty ("eq_hf_atten",       s.mastering().eqHfAtten.load());
    mast->setProperty ("eq_tube_drive",     s.mastering().eqTubeDrive.load());
    mast->setProperty ("eq_output_gain_db", s.mastering().eqOutputGainDb.load());
    mast->setProperty ("comp_enabled",      s.mastering().compEnabled.load());
    mast->setProperty ("comp_thresh_db",    s.mastering().compThreshDb.load());
    mast->setProperty ("comp_ratio",        s.mastering().compRatio.load());
    mast->setProperty ("comp_attack_ms",    s.mastering().compAttackMs.load());
    mast->setProperty ("comp_release_ms",   s.mastering().compReleaseMs.load());
    mast->setProperty ("comp_makeup_db",    s.mastering().compMakeupDb.load());
    mast->setProperty ("limiter_enabled",   s.mastering().limiterEnabled.load());
    mast->setProperty ("limiter_drive_db",  s.mastering().limiterDriveDb.load());
    mast->setProperty ("limiter_ceiling_db",s.mastering().limiterCeilingDb.load());
    mast->setProperty ("limiter_release_ms",s.mastering().limiterReleaseMs.load());
    mast->setProperty ("target_preset",     s.mastering().targetPresetIndex.load());
    root->setProperty ("mastering", juce::var (mast));

    // Transport (loop + punch). Mirrored onto Session by
    // AudioEngine::publishTransportStateForSave before this call runs.
    auto* tport = new juce::DynamicObject();
    tport->setProperty ("loop_enabled",  s.savedLoopEnabled);
    tport->setProperty ("loop_start",    (juce::int64) s.savedLoopStart);
    tport->setProperty ("loop_end",      (juce::int64) s.savedLoopEnd);
    tport->setProperty ("punch_enabled", s.savedPunchEnabled);
    tport->setProperty ("punch_in",      (juce::int64) s.savedPunchIn);
    tport->setProperty ("punch_out",     (juce::int64) s.savedPunchOut);
    tport->setProperty ("snap_to_grid",      s.snapToGrid);
    tport->setProperty ("tempo_bpm",         s.tempoBpm.load());
    tport->setProperty ("beats_per_bar",     s.beatsPerBar.load());
    tport->setProperty ("beat_unit",         s.beatUnit.load());
    tport->setProperty ("metronome_enabled", s.metronomeEnabled.load());
    tport->setProperty ("metronome_vol_db",  s.metronomeVolDb.load());
    tport->setProperty ("count_in_enabled",  s.countInEnabled.load());
    tport->setProperty ("oversampling_factor", s.oversamplingFactor.load());
    root->setProperty ("transport", juce::var (tport));

    const juce::String json = juce::JSON::toString (juce::var (root), false /*allOnOneLine*/);

    // Atomic write: temp file + move-to-target. JUCE's File::moveFileTo uses
    // POSIX rename() under the hood which is atomic on the same filesystem,
    // so a partial session.json never appears even if we crash mid-save.
    target.getParentDirectory().createDirectory();
    auto tmp = target.getParentDirectory().getChildFile (target.getFileName() + ".tmp");
    {
        juce::FileOutputStream out (tmp);
        if (! out.openedOk()) return false;
        out.setPosition (0);
        if (! out.truncate().wasOk())                 return false;
        if (! out.writeText (json, false, false, "\n")) return false;
        out.flush();
        if (out.getStatus().failed())                  return false;
    }
    return tmp.moveFileTo (target);
}

bool SessionSerializer::load (Session& s, const juce::File& source)
{
    if (! source.existsAsFile()) return false;
    juce::var root = juce::JSON::parse (source);
    if (! root.isObject()) return false;

    if (auto tracks = root["tracks"]; tracks.isArray())
    {
        const int n = juce::jmin (Session::kNumTracks, tracks.size());
        for (int i = 0; i < n; ++i)
            restoreTrack (s.track (i), tracks[i]);
    }
    if (auto busesArr = root["buses"]; busesArr.isArray())
    {
        const int n = juce::jmin (Session::kNumBuses, busesArr.size());
        for (int i = 0; i < n; ++i)
            restoreBus (s.bus (i), busesArr[i]);
    }
    if (auto auxLanesArr = root["aux_lanes"]; auxLanesArr.isArray())
    {
        const int n = juce::jmin (Session::kNumAuxLanes, auxLanesArr.size());
        for (int i = 0; i < n; ++i)
        {
            const auto v = auxLanesArr[i];
            if (! v.isObject()) continue;
            auto& lane = s.auxLane (i);
            if (auto str = v["name"].toString();   str.isNotEmpty()) lane.name   = str;
            if (auto str = v["colour"].toString(); str.isNotEmpty()) lane.colour = hexToColour (str, lane.colour);
            if (v.hasProperty ("return_level_db"))
                lane.params.returnLevelDb.store ((float) (double) v["return_level_db"]);
            if (v.hasProperty ("mute"))
                lane.params.mute.store ((bool) v["mute"]);
            if (auto slots = v["plugin_slots"]; slots.isArray())
            {
                const int sn = juce::jmin (AuxLaneParams::kMaxLanePlugins, slots.size());
                for (int p = 0; p < sn; ++p)
                {
                    auto sv = slots[p];
                    if (! sv.isObject()) continue;
                    lane.pluginDescriptionXml[(size_t) p] = sv["plugin_desc_xml"].toString();
                    lane.pluginStateBase64[(size_t) p]    = sv["plugin_state"]   .toString();
                }
            }
        }
    }
    if (auto markersArr = root["markers"]; markersArr.isArray())
    {
        s.getMarkers().clear();
        for (int i = 0; i < markersArr.size(); ++i)
        {
            auto v = markersArr[i];
            if (! v.isObject()) continue;
            // Use the public addMarker so the inserted-sorted invariant
            // holds even if the JSON happened to be out of order.
            const auto idx = s.addMarker ((juce::int64) v["time"], v["name"].toString());
            if (auto col = v["colour"].toString(); col.isNotEmpty())
                s.getMarkers()[(size_t) idx].colour = hexToColour (col, juce::Colour (0xffe0a050));
        }
    }
    if (auto master = root["master"]; master.isObject())
    {
        if (master.hasProperty ("fader_db"))     s.master().faderDb.store ((float) (double) master["fader_db"]);
        if (master.hasProperty ("tape_enabled")) s.master().tapeEnabled.store ((bool) master["tape_enabled"]);
        if (master.hasProperty ("tape_hq"))      s.master().tapeHQ.store ((bool) master["tape_hq"]);
    }
    if (auto mast = root["mastering"]; mast.isObject())
    {
        auto& m = s.mastering();
        if (mast.hasProperty ("source_file"))
        {
            const juce::String p = mast["source_file"].toString();
            m.sourceFile = p.isNotEmpty() ? juce::File (p) : juce::File();
        }
        auto loadF = [&] (const char* k, std::atomic<float>& dst)
            { if (mast.hasProperty (k)) dst.store ((float) (double) mast[k]); };
        auto loadB = [&] (const char* k, std::atomic<bool>& dst)
            { if (mast.hasProperty (k)) dst.store ((bool) mast[k]); };
        loadB ("eq_enabled",        m.eqEnabled);
        loadF ("eq_lf_boost",       m.eqLfBoost);
        loadF ("eq_hf_boost",       m.eqHfBoost);
        loadF ("eq_hf_atten",       m.eqHfAtten);
        loadF ("eq_tube_drive",     m.eqTubeDrive);
        loadF ("eq_output_gain_db", m.eqOutputGainDb);
        loadB ("comp_enabled",      m.compEnabled);
        loadF ("comp_thresh_db",    m.compThreshDb);
        loadF ("comp_ratio",        m.compRatio);
        loadF ("comp_attack_ms",    m.compAttackMs);
        loadF ("comp_release_ms",   m.compReleaseMs);
        loadF ("comp_makeup_db",    m.compMakeupDb);
        loadB ("limiter_enabled",   m.limiterEnabled);
        loadF ("limiter_drive_db",  m.limiterDriveDb);
        loadF ("limiter_ceiling_db",m.limiterCeilingDb);
        loadF ("limiter_release_ms",m.limiterReleaseMs);
        if (mast.hasProperty ("target_preset"))
            m.targetPresetIndex.store ((int) mast["target_preset"]);
    }
    if (auto tport = root["transport"]; tport.isObject())
    {
        if (tport.hasProperty ("loop_enabled"))  s.savedLoopEnabled  = (bool) tport["loop_enabled"];
        if (tport.hasProperty ("loop_start"))    s.savedLoopStart    = (juce::int64) tport["loop_start"];
        if (tport.hasProperty ("loop_end"))      s.savedLoopEnd      = (juce::int64) tport["loop_end"];
        if (tport.hasProperty ("punch_enabled")) s.savedPunchEnabled = (bool) tport["punch_enabled"];
        if (tport.hasProperty ("punch_in"))      s.savedPunchIn      = (juce::int64) tport["punch_in"];
        if (tport.hasProperty ("punch_out"))     s.savedPunchOut     = (juce::int64) tport["punch_out"];
        if (tport.hasProperty ("snap_to_grid"))  s.snapToGrid        = (bool) tport["snap_to_grid"];
        if (tport.hasProperty ("tempo_bpm"))         s.tempoBpm.store         ((float) (double) tport["tempo_bpm"]);
        if (tport.hasProperty ("beats_per_bar"))     s.beatsPerBar.store      ((int)    tport["beats_per_bar"]);
        if (tport.hasProperty ("beat_unit"))         s.beatUnit.store         ((int)    tport["beat_unit"]);
        if (tport.hasProperty ("metronome_enabled")) s.metronomeEnabled.store ((bool)   tport["metronome_enabled"]);
        if (tport.hasProperty ("metronome_vol_db"))  s.metronomeVolDb.store   ((float) (double) tport["metronome_vol_db"]);
        if (tport.hasProperty ("count_in_enabled"))  s.countInEnabled.store   ((bool)   tport["count_in_enabled"]);
        if (tport.hasProperty ("oversampling_factor"))
        {
            const int f = (int) tport["oversampling_factor"];
            s.oversamplingFactor.store ((f == 2 || f == 4) ? f : 1, std::memory_order_relaxed);
        }
    }
    // Bulk load wrote solo / armed atoms directly - resync the RT counters
    // so the audio thread's any-X-soloed reads are correct on first callback.
    s.recomputeRtCounters();
    return true;
}
} // namespace focal
