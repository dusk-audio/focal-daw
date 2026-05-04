#include "SessionSerializer.h"

namespace adhdaw
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

    obj->setProperty ("fader_db",       t.strip.faderDb.load());
    obj->setProperty ("pan",            t.strip.pan.load());
    obj->setProperty ("mute",           t.strip.mute.load());
    obj->setProperty ("solo",           t.strip.solo.load());
    obj->setProperty ("phase_invert",   t.strip.phaseInvert.load());
    obj->setProperty ("input_monitor",  t.inputMonitor.load());
    obj->setProperty ("print_effects",  t.printEffects.load());
    obj->setProperty ("input_source",   t.inputSource.load());

    juce::Array<juce::var> buses;
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
        buses.add (t.strip.busAssign[(size_t) i].load());
    obj->setProperty ("bus_assign", buses);

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
    // Per-mode parameters — UniversalCompressor's native shape.
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
        regions.add (juce::var (rObj));
    }
    obj->setProperty ("regions", regions);

    return obj;
}

juce::DynamicObject::Ptr auxToObject (const AuxBus& a)
{
    auto* obj = new juce::DynamicObject();
    obj->setProperty ("name",     a.name);
    obj->setProperty ("colour",   colourToHex (a.colour));
    obj->setProperty ("fader_db", a.strip.faderDb.load());
    obj->setProperty ("mute",     a.strip.mute.load());
    obj->setProperty ("solo",     a.strip.solo.load());
    return obj;
}

void restoreTrack (Track& t, const juce::var& v)
{
    if (! v.isObject()) return;
    if (auto s = v["name"].toString();           s.isNotEmpty()) t.name = s;
    if (auto s = v["colour"].toString();         s.isNotEmpty()) t.colour = hexToColour (s, t.colour);

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

    if (auto buses = v["bus_assign"]; buses.isArray())
    {
        const int n = juce::jmin (ChannelStripParams::kNumBuses, buses.size());
        for (int i = 0; i < n; ++i)
            t.strip.busAssign[(size_t) i].store ((bool) buses[i], std::memory_order_relaxed);
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
            t.regions.push_back (std::move (r));
        }
    }
}

void restoreAux (AuxBus& a, const juce::var& v)
{
    if (! v.isObject()) return;
    if (auto s = v["name"].toString();   s.isNotEmpty()) a.name = s;
    if (auto s = v["colour"].toString(); s.isNotEmpty()) a.colour = hexToColour (s, a.colour);
    if (v.hasProperty ("fader_db"))                       a.strip.faderDb.store ((float) (double) v["fader_db"]);
    if (v.hasProperty ("mute"))                           a.strip.mute.store ((bool) v["mute"]);
    if (v.hasProperty ("solo"))                           a.strip.solo.store ((bool) v["solo"]);
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

    juce::Array<juce::var> auxes;
    for (int i = 0; i < Session::kNumAuxBuses; ++i)
        auxes.add (juce::var (auxToObject (s.aux (i)).get()));
    root->setProperty ("aux_buses", auxes);

    auto* master = new juce::DynamicObject();
    master->setProperty ("fader_db",     s.master().faderDb.load());
    master->setProperty ("tape_enabled", s.master().tapeEnabled.load());
    master->setProperty ("tape_hq",      s.master().tapeHQ.load());
    root->setProperty ("master", juce::var (master));

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
    if (auto auxes = root["aux_buses"]; auxes.isArray())
    {
        const int n = juce::jmin (Session::kNumAuxBuses, auxes.size());
        for (int i = 0; i < n; ++i)
            restoreAux (s.aux (i), auxes[i]);
    }
    if (auto master = root["master"]; master.isObject())
    {
        if (master.hasProperty ("fader_db"))     s.master().faderDb.store ((float) (double) master["fader_db"]);
        if (master.hasProperty ("tape_enabled")) s.master().tapeEnabled.store ((bool) master["tape_enabled"]);
        if (master.hasProperty ("tape_hq"))      s.master().tapeHQ.store ((bool) master["tape_hq"]);
    }
    return true;
}
} // namespace adhdaw
