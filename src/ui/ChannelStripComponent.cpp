#include "ChannelStripComponent.h"
#include "FocalLookAndFeel.h"
#include "ChannelEqEditor.h"
#include "ChannelCompEditor.h"
#include "../engine/AudioEngine.h"
#include "../engine/PluginSlot.h"
#include "../engine/PluginManager.h"

namespace focal
{
namespace
{
struct BandSpec
{
    const char* rowName;
    juce::Colour accent;          // 4K-palette band color
    float freqMin, freqMax, freqDefault;
    std::atomic<float>* (*gainPtr) (ChannelStripParams&);
    std::atomic<float>* (*freqPtr) (ChannelStripParams&);
    // qPtr is non-null for bell-only mid bands (HM, LM). Shelf bands (HF, LF)
    // leave it null and get a 2-knob row instead of 3.
    std::atomic<float>* (*qPtr)    (ChannelStripParams&) = nullptr;
};

// Top-to-bottom order: HF (treble) on top, LF (bass) on the bottom - matches
// the SSL/console convention and the user's spatial expectation. HF and LF
// are shelf-only here (no Q), HM and LM are bell with Q exposed - three
// knobs per row, mirroring the SSL E-EQ layout.
static const std::array<BandSpec, 4>& bandSpecs()
{
    static const std::array<BandSpec, 4> specs {{
        { "HF", juce::Colour (sslEqColors::kHfRed),    ChannelStripParams::kHfFreqMin, ChannelStripParams::kHfFreqMax, 8000.0f,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hfGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hfFreq; } },
        { "HM", juce::Colour (sslEqColors::kHmGreen),  ChannelStripParams::kHmFreqMin, ChannelStripParams::kHmFreqMax, 2000.0f,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmFreq; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.hmQ; } },
        { "LM", juce::Colour (sslEqColors::kLmBlue),   ChannelStripParams::kLmFreqMin, ChannelStripParams::kLmFreqMax, 600.0f,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmFreq; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lmQ; } },
        { "LF", juce::Colour (sslEqColors::kLfBlack),  ChannelStripParams::kLfFreqMin, ChannelStripParams::kLfFreqMax, 100.0f,
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lfGainDb; },
            [] (ChannelStripParams& s) -> std::atomic<float>* { return &s.lfFreq; } },
    }};
    return specs;
}

void styleCompactKnob (juce::Slider& k, juce::Colour fill)
{
    k.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    k.setColour (juce::Slider::rotarySliderFillColourId, fill);
    k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
}

void enableValueLabel (juce::Slider& k, const juce::String& suffix, int decimals,
                       int width = 56, int height = 14)
{
    k.setTextBoxStyle (juce::Slider::TextBoxBelow, true, width, height);
    k.setTextValueSuffix (suffix);
    k.setNumDecimalPlacesToDisplay (decimals);
    k.setColour (juce::Slider::textBoxTextColourId,       juce::Colour (0xffd8d8d8));
    k.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0));
    k.setColour (juce::Slider::textBoxOutlineColourId,    juce::Colour (0));
}

// Format a frequency in Hz, switching to "1.2k" notation above 1 kHz and
// dropping a trailing ".0" so integer kHz values stay short ("2k", "8k")
// instead of "2.0k" / "8.0k". Tight-strip-friendly.
inline juce::String formatFrequency (double hz)
{
    if (hz >= 1000.0)
    {
        const double khz = hz / 1000.0;
        if (std::abs (khz - std::round (khz)) < 0.05)
            return juce::String ((int) std::round (khz)) + "k";
        return juce::String (khz, 1) + "k";
    }
    return juce::String ((int) std::round (hz));
}

// Format an EQ band gain in dB, dropping ".0" on integer values so "0" / "-2"
// / "+12" fit in narrow textboxes instead of "0.0" / "-2.0" / "+12.0".
inline juce::String formatBandGain (double db)
{
    const double rounded = std::round (db);
    if (std::abs (db - rounded) < 0.05)
    {
        const int idb = (int) rounded;
        if (idb > 0) return "+" + juce::String (idb);
        return juce::String (idb);
    }
    return juce::String (db, 1);
}
} // namespace

// Style helper for the EQ / COMP compact-mode placeholder buttons. They sit
// in the slots the inline EQ + COMP sections occupy in normal mode and
// open the corresponding editor as a popup when clicked. Hidden by default.
static void styleCompactSectionButton (juce::TextButton& b, juce::Colour accent)
{
    b.setClickingTogglesState (false);
    b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222226));
    b.setColour (juce::TextButton::textColourOffId,  accent.brighter (0.3f));
    b.setColour (juce::TextButton::textColourOnId,   accent.brighter (0.3f));
    b.setVisible (false);
}

ChannelStripComponent::ChannelStripComponent (int idx, Track& t, Session& s,
                                                PluginSlot& slot, AudioEngine& eng)
    : trackIndex (idx), track (t), session (s), pluginSlot (slot), engine (eng)
{
    nameLabel.setText (track.name, juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    nameLabel.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    nameLabel.setEditable (false, true, false);  // single-click no, double-click YES, no submit-on-empty
    nameLabel.setTooltip (juce::CharPointer_UTF8 (
        "Double-click to rename · right-click for colour"));
    nameLabel.onTextChange = [this]
    {
        auto txt = nameLabel.getText().trim();
        if (txt.isEmpty()) txt = juce::String (trackIndex + 1);
        track.name = txt;
        nameLabel.setText (txt, juce::dontSendNotification);
    };
    addAndMakeVisible (nameLabel);

    // ── HPF row label (now the first row inside the EQ block, styled like
    // the band row labels: centred, 12 pt bold, accent colour) ──
    hpfLabel.setText ("HPF", juce::dontSendNotification);
    hpfLabel.setJustificationType (juce::Justification::centred);
    hpfLabel.setColour (juce::Label::textColourId, juce::Colour (sslEqColors::kHpfBlue).brighter (0.2f));
    hpfLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
    addAndMakeVisible (hpfLabel);

    hpfKnob.setRange (ChannelStripParams::kHpfMinHz, ChannelStripParams::kHpfMaxHz, 1.0);
    hpfKnob.setSkewFactorFromMidPoint (80.0);
    hpfKnob.setDoubleClickReturnValue (true, ChannelStripParams::kHpfOffHz);
    hpfKnob.setTooltip ("HPF cutoff (turn fully down to bypass)");
    styleCompactKnob (hpfKnob, juce::Colour (sslEqColors::kHpfBlue));
    enableValueLabel (hpfKnob, "", 0);
    hpfKnob.textFromValueFunction = [] (double v) -> juce::String
    {
        if (v <= ChannelStripParams::kHpfOffHz + 0.5) return "OFF";
        return formatFrequency (v);
    };
    hpfKnob.setValue (track.strip.hpfFreq.load (std::memory_order_relaxed), juce::dontSendNotification);
    hpfKnob.onValueChange = [this] { onHpfKnobChanged(); };
    addAndMakeVisible (hpfKnob);

    // ── EQ region ──
    eqHeaderLabel.setText ("EQ", juce::dontSendNotification);
    eqHeaderLabel.setJustificationType (juce::Justification::centredLeft);
    eqHeaderLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd0d0d0));
    eqHeaderLabel.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    addAndMakeVisible (eqHeaderLabel);

    eqTypeButton.setClickingTogglesState (true);
    eqTypeButton.setToggleState (track.strip.eqBlackMode.load (std::memory_order_relaxed),
                                  juce::dontSendNotification);
    eqTypeButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff5a3a20));   // brown
    eqTypeButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff202020));   // black
    eqTypeButton.setColour (juce::TextButton::textColourOffId,  juce::Colours::white);
    eqTypeButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    eqTypeButton.setTooltip ("Brown (E-series) / Black (G-series)");
    eqTypeButton.onClick = [this]
    {
        const bool isBlack = eqTypeButton.getToggleState();
        track.strip.eqBlackMode.store (isBlack, std::memory_order_relaxed);
        refreshEqTypeButton();
    };
    refreshEqTypeButton();
    addAndMakeVisible (eqTypeButton);

    // ── COMP region ──
    compOnButton.setClickingTogglesState (true);
    compOnButton.setToggleState (track.strip.compEnabled.load (std::memory_order_relaxed),
                                  juce::dontSendNotification);
    compOnButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    compOnButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd09060));
    compOnButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd09060));
    compOnButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    compOnButton.setTooltip ("Compressor on/off");
    compOnButton.onClick = [this]
    {
        track.strip.compEnabled.store (compOnButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (compOnButton);

    auto styleModeButton = [] (juce::TextButton& b)
    {
        b.setClickingTogglesState (true);
        b.setRadioGroupId (10, juce::dontSendNotification);
        b.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff181820));
        b.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd09060));
        b.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd09060));
        b.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    };
    styleModeButton (compModeOpto);
    styleModeButton (compModeFet);
    styleModeButton (compModeVca);
    compModeOpto.setTooltip ("Opto: program-dependent, smooth");
    compModeFet.setTooltip  ("FET: fast attack, gritty under load");
    compModeVca.setTooltip  ("VCA: clean, predictable");
    compModeOpto.onClick = [this] { setCompMode (0); };
    compModeFet.onClick  = [this] { setCompMode (1); };
    compModeVca.onClick  = [this] { setCompMode (2); };
    addAndMakeVisible (compModeOpto);
    addAndMakeVisible (compModeFet);
    addAndMakeVisible (compModeVca);
    refreshCompModeButtons();

    auto setupCompKnob = [] (juce::Slider& k, juce::Label& label, const juce::String& labelText,
                              double rangeMin, double rangeMax, double defaultVal,
                              double skewMid, const juce::String& tooltip,
                              std::atomic<float>& target,
                              const juce::String& valueSuffix, int decimals)
    {
        k.setRange (rangeMin, rangeMax, 0.01);
        if (skewMid > 0) k.setSkewFactorFromMidPoint (skewMid);
        k.setValue (target.load (std::memory_order_relaxed), juce::dontSendNotification);
        k.setDoubleClickReturnValue (true, defaultVal);
        k.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (fourKColors::kCompGold));
        k.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
        k.setTooltip (tooltip);
        enableValueLabel (k, valueSuffix, decimals);
        k.onValueChange = [&k, &target] { target.store ((float) k.getValue(), std::memory_order_relaxed); };

        label.setText (labelText, juce::dontSendNotification);
        label.setJustificationType (juce::Justification::centred);
        label.setColour (juce::Label::textColourId, juce::Colour (0xffb07050));
        label.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
    };

    // ── Opto knobs (LA-2A: peak reduction + gain + LIMIT toggle) ──
    setupCompKnob (optoPeakRedKnob, optoPeakRedLabel, "PEAK",
                   0.0, 100.0, 30.0, 0.0, "Peak Reduction (%)",
                   track.strip.compOptoPeakRed, "", 0);
    setupCompKnob (optoGainKnob, optoGainLabel, "GAIN",
                   0.0, 100.0, 50.0, 0.0, "Output gain (% - 50 = unity)",
                   track.strip.compOptoGain, "", 0);
    addAndMakeVisible (optoPeakRedKnob);  addAndMakeVisible (optoPeakRedLabel);
    addAndMakeVisible (optoGainKnob);     addAndMakeVisible (optoGainLabel);

    optoLimitButton.setClickingTogglesState (true);
    optoLimitButton.setToggleState (track.strip.compOptoLimit.load (std::memory_order_relaxed),
                                     juce::dontSendNotification);
    optoLimitButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff181820));
    optoLimitButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd09060));
    optoLimitButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd09060));
    optoLimitButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    optoLimitButton.setTooltip ("Limit mode (more aggressive than Compress)");
    optoLimitButton.onClick = [this]
    {
        track.strip.compOptoLimit.store (optoLimitButton.getToggleState(),
                                          std::memory_order_relaxed);
    };
    addAndMakeVisible (optoLimitButton);

    // ── FET knobs (1176: in/out/atk/rel + ratio button row) ──
    setupCompKnob (fetInputKnob,   fetInputLabel,   "IN",
                   -20.0, 40.0, 0.0, 0.0, "Input drive (dB)",
                   track.strip.compFetInput, "", 1);
    setupCompKnob (fetOutputKnob,  fetOutputLabel,  "OUT",
                   -20.0, 20.0, 0.0, 0.0, "Output gain (dB)",
                   track.strip.compFetOutput, "", 1);
    setupCompKnob (fetAttackKnob,  fetAttackLabel,  "ATK",
                   0.02, 80.0, 0.2, 0.5, "Attack (ms)",
                   track.strip.compFetAttack, "", 2);
    setupCompKnob (fetReleaseKnob, fetReleaseLabel, "REL",
                   50.0, 1100.0, 400.0, 300.0, "Release (ms)",
                   track.strip.compFetRelease, "", 0);
    addAndMakeVisible (fetInputKnob);   addAndMakeVisible (fetInputLabel);
    addAndMakeVisible (fetOutputKnob);  addAndMakeVisible (fetOutputLabel);
    addAndMakeVisible (fetAttackKnob);  addAndMakeVisible (fetAttackLabel);
    addAndMakeVisible (fetReleaseKnob); addAndMakeVisible (fetReleaseLabel);

    // FET ratio: 5-step knob (4:1 / 8:1 / 12:1 / 20:1 / All) - replaces the
    // earlier button row which couldn't fit the "All" label in narrow strips.
    fetRatioKnob.setRange (0.0, 4.0, 1.0);
    fetRatioKnob.setValue ((double) track.strip.compFetRatio.load (std::memory_order_relaxed),
                            juce::dontSendNotification);
    fetRatioKnob.setDoubleClickReturnValue (true, 0.0);
    fetRatioKnob.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (fourKColors::kCompGold));
    fetRatioKnob.setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
    fetRatioKnob.setTooltip ("FET Ratio (4 / 8 / 12 / 20 / All-buttons)");
    enableValueLabel (fetRatioKnob, "", 0);
    fetRatioKnob.textFromValueFunction = [] (double v) -> juce::String
    {
        const int idx = juce::jlimit (0, 4, (int) std::round (v));
        const char* names[] = { "4:1", "8:1", "12:1", "20:1", "All" };
        return names[idx];
    };
    fetRatioKnob.onValueChange = [this]
    {
        track.strip.compFetRatio.store (
            juce::jlimit (0, 4, (int) std::round (fetRatioKnob.getValue())),
            std::memory_order_relaxed);
    };
    fetRatioKnob.updateText();
    addAndMakeVisible (fetRatioKnob);

    fetRatioLabel.setText ("RATIO", juce::dontSendNotification);
    fetRatioLabel.setJustificationType (juce::Justification::centred);
    fetRatioLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb07050));
    fetRatioLabel.setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
    addAndMakeVisible (fetRatioLabel);

    // ── VCA knobs (ratio/attack/release/output - threshold lives on the
    //   meter-strip drag handle so the engineer pulls it directly against
    //   the input level, the way a real VCA panel works) ──
    setupCompKnob (vcaRatioKnob,   vcaRatioLabel,   "RAT",
                   1.0, 120.0, 4.0, 4.0, "Ratio (n:1)",
                   track.strip.compVcaRatio, ":1", 1);
    setupCompKnob (vcaAttackKnob,  vcaAttackLabel,  "ATK",
                   0.1, 50.0, 1.0, 5.0, "Attack (ms)",
                   track.strip.compVcaAttack, "", 1);
    setupCompKnob (vcaReleaseKnob, vcaReleaseLabel, "REL",
                   10.0, 5000.0, 100.0, 200.0, "Release (ms)",
                   track.strip.compVcaRelease, "", 0);
    setupCompKnob (vcaOutputKnob,  vcaOutputLabel,  "OUT",
                   -20.0, 20.0, 0.0, 0.0, "Output gain (dB)",
                   track.strip.compVcaOutput, "", 1);
    addAndMakeVisible (vcaRatioKnob);    addAndMakeVisible (vcaRatioLabel);
    addAndMakeVisible (vcaAttackKnob);   addAndMakeVisible (vcaAttackLabel);
    addAndMakeVisible (vcaReleaseKnob);  addAndMakeVisible (vcaReleaseLabel);
    addAndMakeVisible (vcaOutputKnob);   addAndMakeVisible (vcaOutputLabel);

    // Vertical comp metering with threshold drag-handle (Mixbus-style).
    compMeter = std::make_unique<CompMeterStrip> (track);
    addAndMakeVisible (compMeter.get());

    startTimerHz (30);  // input + GR meter refresh on the main strip

    for (size_t i = 0; i < bandSpecs().size(); ++i)
    {
        const auto& spec = bandSpecs()[i];
        auto& row = eqRows[i];

        row.rowLabel.setText (spec.rowName, juce::dontSendNotification);
        row.rowLabel.setJustificationType (juce::Justification::centred);
        row.rowLabel.setColour (juce::Label::textColourId, spec.accent.brighter (0.2f));
        row.rowLabel.setFont (juce::Font (juce::FontOptions (12.0f, juce::Font::bold)));
        addAndMakeVisible (row.rowLabel);

        row.gain = std::make_unique<juce::Slider>();
        styleCompactKnob (*row.gain, spec.accent);
        row.gain->setRange (ChannelStripParams::kBandGainMin, ChannelStripParams::kBandGainMax, 0.1);
        row.gain->setDoubleClickReturnValue (true, 0.0);
        row.gain->setTooltip (juce::String (spec.rowName) + " Gain (dB)");
        enableValueLabel (*row.gain, "", 1);
        row.gain->textFromValueFunction = [] (double v) { return formatBandGain (v); };
        row.gain->setValue (spec.gainPtr (track.strip)->load (std::memory_order_relaxed),
                            juce::dontSendNotification);
        row.gain->updateText();  // setValue skips updateText() when value didn't change (default 0 → loaded 0)
        {
            auto* atomicPtr = spec.gainPtr (track.strip);
            auto* knob = row.gain.get();
            knob->onValueChange = [knob, atomicPtr]
            {
                atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
            };
        }
        addAndMakeVisible (row.gain.get());

        row.freq = std::make_unique<juce::Slider>();
        styleCompactKnob (*row.freq, spec.accent);
        row.freq->setRange (spec.freqMin, spec.freqMax, 1.0);
        row.freq->setSkewFactorFromMidPoint ((double) spec.freqDefault);
        row.freq->setDoubleClickReturnValue (true, spec.freqDefault);
        row.freq->setTooltip (juce::String (spec.rowName) + " Frequency (Hz)");
        enableValueLabel (*row.freq, "", 0);
        // textFromValueFunction must be set BEFORE setValue, otherwise the
        // initial text is rendered with the default formatter ("2000") and
        // doesn't get our "2.0k" notation until the user moves the knob.
        row.freq->textFromValueFunction = [] (double v) { return formatFrequency (v); };
        row.freq->setValue (spec.freqPtr (track.strip)->load (std::memory_order_relaxed),
                            juce::dontSendNotification);

        // Q knob (mid-bands only - bell-only HM/LM). Same size and styling
        // as gain and freq, with a "Q 0.7"-style value label below so the
        // user can read the bandwidth without hovering.
        if (spec.qPtr != nullptr)
        {
            row.q = std::make_unique<juce::Slider>();
            styleCompactKnob (*row.q, spec.accent);
            row.q->setRange (ChannelStripParams::kBandQMin, ChannelStripParams::kBandQMax, 0.01);
            row.q->setSkewFactorFromMidPoint (1.0);  // Q midpoint at 1.0
            row.q->setDoubleClickReturnValue (true, 0.7);
            row.q->setTooltip (juce::String (spec.rowName) + " Q (bandwidth)");
            enableValueLabel (*row.q, "", 1);
            row.q->setValue (spec.qPtr (track.strip)->load (std::memory_order_relaxed),
                              juce::dontSendNotification);
            {
                auto* atomicPtr = spec.qPtr (track.strip);
                auto* knob = row.q.get();
                knob->onValueChange = [knob, atomicPtr]
                {
                    atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
                };
            }
            addAndMakeVisible (row.q.get());

            row.qLabel.setText ("Q", juce::dontSendNotification);
            row.qLabel.setJustificationType (juce::Justification::centred);
            row.qLabel.setColour (juce::Label::textColourId, spec.accent.brighter (0.2f));
            row.qLabel.setFont (juce::Font (juce::FontOptions (10.0f, juce::Font::bold)));
            addAndMakeVisible (row.qLabel);
        }
        {
            auto* atomicPtr = spec.freqPtr (track.strip);
            auto* knob = row.freq.get();
            knob->onValueChange = [knob, atomicPtr]
            {
                atomicPtr->store ((float) knob->getValue(), std::memory_order_relaxed);
            };
        }
        addAndMakeVisible (row.freq.get());
    }

    // ── Bus assigns - on/off toggles laid out vertically alongside the fader.
    // Each button takes the colour of its corresponding aux bus header (the
    // same HSV ramp Session::Session() uses to colour the four aux strips),
    // so the engineer reads "BUS 1 = teal, BUS 2 = blue, …" without consulting
    // the strip headers. ──
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
    {
        const auto busColour = session.bus (i).colour;
        lastBusColours[(size_t) i] = busColour.getARGB();
        auto btn = std::make_unique<juce::TextButton> (juce::String (i + 1));
        btn->setClickingTogglesState (true);
        btn->setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
        btn->setColour (juce::TextButton::buttonOnColourId, busColour);
        btn->setColour (juce::TextButton::textColourOffId,  busColour.brighter (0.15f));
        btn->setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
        btn->setToggleState (track.strip.busAssign[(size_t) i].load (std::memory_order_relaxed),
                              juce::dontSendNotification);
        btn->setTooltip ("Route post-fader signal to BUS " + juce::String (i + 1));
        btn->onClick = [this, i]
        {
            track.strip.busAssign[(size_t) i].store (busButtons[(size_t) i]->getToggleState(),
                                                      std::memory_order_relaxed);
        };
        addAndMakeVisible (btn.get());
        busButtons[(size_t) i] = std::move (btn);
    }

    // ── Pan / Fader / M / S ──
    panLabel.setText ("PAN", juce::dontSendNotification);
    panLabel.setJustificationType (juce::Justification::centred);
    panLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe07070));
    panLabel.setFont (juce::Font (juce::FontOptions (10.5f, juce::Font::bold)));
    addAndMakeVisible (panLabel);

    panKnob.setRange (-1.0, 1.0, 0.001);
    panKnob.setDoubleClickReturnValue (true, 0.0);
    panKnob.setColour (juce::Slider::rotarySliderFillColourId, juce::Colour (0xffc04040));  // red pan
    enableValueLabel (panKnob, "", 0);
    panKnob.textFromValueFunction = [] (double v) -> juce::String
    {
        if (std::abs (v) < 0.01) return "C";
        const int pct = (int) std::round (std::abs (v) * 100.0);
        return (v < 0 ? "L" : "R") + juce::String (pct);
    };
    panKnob.setValue (track.strip.pan.load (std::memory_order_relaxed), juce::dontSendNotification);
    panKnob.onValueChange = [this]
    {
        track.strip.pan.store ((float) panKnob.getValue(), std::memory_order_relaxed);
    };
    panKnob.onDragStart = [this]
    {
        track.strip.panTouched.store (true, std::memory_order_release);
    };
    panKnob.onDragEnd = [this]
    {
        track.strip.panTouched.store (false, std::memory_order_release);
    };
    addAndMakeVisible (panKnob);

    faderSlider.setRange (ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb, 0.1);
    faderSlider.setSkewFactorFromMidPoint (-12.0);
    faderSlider.setValue (track.strip.faderDb.load (std::memory_order_relaxed), juce::dontSendNotification);
    faderSlider.setDoubleClickReturnValue (true, 0.0);
    // No "dB" suffix - strip is narrow enough that "0.0 dB" truncates.
    // The dB scale column to the right of the meter makes the unit obvious.
    faderSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 50, 16);
    faderSlider.setTooltip ("Channel fader (dB) - double-click to reset to 0 dB");
    faderSlider.onValueChange = [this]
    {
        track.strip.faderDb.store ((float) faderSlider.getValue(), std::memory_order_relaxed);
    };
    // Touch-mode hooks: while the user has the fader grabbed, set the
    // strip's faderTouched flag so the audio engine routes the manual
    // setpoint instead of the lane (and the timerCallback captures into
    // the lane while touched). On release, the existing fader smoother's
    // 20 ms ramp blends from manual back to lane - a fast but smooth
    // glide. The configurable 100 ms / 500 ms / 1 s glide-back from the
    // spec is a refinement that lands later if 20 ms feels jarring in
    // practice.
    faderSlider.onDragStart = [this]
    {
        track.strip.faderTouched.store (true, std::memory_order_release);
    };
    faderSlider.onDragEnd = [this]
    {
        track.strip.faderTouched.store (false, std::memory_order_release);
    };
    addAndMakeVisible (faderSlider);

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
    muteButton.setToggleState (track.strip.mute.load (std::memory_order_relaxed), juce::dontSendNotification);
    muteButton.setTooltip ("Mute (M) - silences this channel at the master");
    muteButton.onClick = [this]
    {
        track.strip.mute.store (muteButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (muteButton);

    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::yellow.darker (0.2f));
    soloButton.setToggleState (track.strip.solo.load (std::memory_order_relaxed), juce::dontSendNotification);
    soloButton.setTooltip ("Solo (S) - when any channel is soloed, only soloed channels are heard");
    soloButton.onClick = [this]
    {
        // Route through the session-level setter so the counter-backed
        // anyTrackSoloed() stays in sync with the underlying atom.
        session.setTrackSoloed (trackIndex, soloButton.getToggleState());
    };
    addAndMakeVisible (soloButton);

    phaseButton.setClickingTogglesState (true);
    phaseButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff70c0d0));
    phaseButton.setToggleState (track.strip.phaseInvert.load (std::memory_order_relaxed),
                                 juce::dontSendNotification);
    phaseButton.setTooltip (juce::CharPointer_UTF8 (
        "Polarity invert (Ø) - flips the input signal's polarity"));
    phaseButton.onClick = [this]
    {
        track.strip.phaseInvert.store (phaseButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (phaseButton);

    autoModeButton.setTooltip ("Automation: OFF -> READ -> WRITE -> TOUCH -> OFF. "
                                "READ replays the recorded ride; WRITE captures "
                                "fader+pan moves; TOUCH replays until you grab a "
                                "control, then captures while held.");
    autoModeButton.onClick = [this] { onAutoModeClicked(); };
    addAndMakeVisible (autoModeButton);
    refreshAutoModeButton();
    displayedLiveFaderDb = track.strip.liveFaderDb.load (std::memory_order_relaxed);
    displayedLivePan     = track.strip.livePan    .load (std::memory_order_relaxed);

    // ── Peak input level readout ──
    inputPeakLabel.setJustificationType (juce::Justification::centred);
    inputPeakLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd0d0d0));
    inputPeakLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff0a0a0c));
    // No outline - the label sat next to the fader's value textbox and the
    // 1 px border drew on top of the textbox edge, looking like overlap.
    inputPeakLabel.setColour (juce::Label::outlineColourId,    juce::Colours::transparentBlack);
    inputPeakLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                            13.0f, juce::Font::bold)));
    inputPeakLabel.setText ("-inf", juce::dontSendNotification);
    addAndMakeVisible (inputPeakLabel);

    // GR readout - sits to the right of the input peak. Negative dB when
    // the comp is pulling the signal down. Uses gold-on-black to read as a
    // comp-section indicator distinct from the input level.
    grPeakLabel.setJustificationType (juce::Justification::centred);
    grPeakLabel.setColour (juce::Label::textColourId,        juce::Colour (0xffe0c050));
    grPeakLabel.setColour (juce::Label::backgroundColourId,  juce::Colour (0xff0a0a0c));
    grPeakLabel.setColour (juce::Label::outlineColourId,     juce::Colours::transparentBlack);
    grPeakLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                          12.0f, juce::Font::bold)));
    grPeakLabel.setText ("0.0", juce::dontSendNotification);
    grPeakLabel.setTooltip ("Gain reduction in dB (negative = comp pulling down). "
                             "Goes inert when the comp is bypassed.");
    addAndMakeVisible (grPeakLabel);

    // "THR" label sits above CompMeterStrip's drag handle so the engineer
    // sees what the triangle pointer controls. Same gold tone as the COMP
    // section labels so it reads as part of the comp UI.
    threshMeterLabel.setText ("THR", juce::dontSendNotification);
    threshMeterLabel.setJustificationType (juce::Justification::centred);
    threshMeterLabel.setColour (juce::Label::textColourId, juce::Colour (0xffb07050));
    threshMeterLabel.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
    addAndMakeVisible (threshMeterLabel);

    // ── Input monitor toggle (IN) ──
    monitorButton.setClickingTogglesState (true);
    monitorButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    monitorButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (fourKColors::kPanCyan));
    monitorButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff708090));
    monitorButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    monitorButton.setToggleState (track.inputMonitor.load (std::memory_order_relaxed),
                                   juce::dontSendNotification);
    monitorButton.setTooltip ("Input monitor - when on, live input is heard at the master. "
                              "When off, the track still records and meters but is silent.");
    monitorButton.onClick = [this]
    {
        track.inputMonitor.store (monitorButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (monitorButton);

    // ── Record arm ──
    armButton.setClickingTogglesState (true);
    armButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    armButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd03030));
    armButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xffd06060));
    armButton.setColour (juce::TextButton::textColourOnId,   juce::Colours::white);
    armButton.setToggleState (track.recordArmed.load (std::memory_order_relaxed),
                               juce::dontSendNotification);
    armButton.setTooltip ("Record arm - press REC on the transport to capture this input");
    armButton.onClick = [this]
    {
        session.setTrackArmed (trackIndex, armButton.getToggleState());
    };
    addAndMakeVisible (armButton);

    // PRINT toggle - when on, recording captures the post-EQ/post-comp signal
    // so effects are committed to the WAV. Off (default) = clean input on
    // disk so the engineer can re-EQ / re-comp at mix time.
    printButton.setClickingTogglesState (true);
    printButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    printButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd09060));
    printButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff8a7060));
    printButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    printButton.setToggleState (track.printEffects.load (std::memory_order_relaxed),
                                 juce::dontSendNotification);
    printButton.setTooltip ("PRINT - when on, the recorded WAV captures the post-EQ/post-comp signal "
                             "(effects baked in). Off = clean input recorded; effects only at playback.");
    printButton.onClick = [this]
    {
        track.printEffects.store (printButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (printButton);

    // ── Input selector ──
    // -2 = follow track index; -1 = none; 0..N = explicit input. We populate
    // a small set of options here; the device may have fewer inputs at runtime
    // and we'll just route silence in that case.
    inputSelector.addItem ("In " + juce::String (trackIndex + 1), 1);   // ID 1 = follow (-2)
    inputSelector.addItem ("None",                                  2); // ID 2 = -1
    for (int i = 0; i < 16; ++i)
        inputSelector.addItem ("In " + juce::String (i + 1) + " (fixed)", 100 + i);  // ID 100+i = explicit

    const int currentSrc = track.inputSource.load (std::memory_order_relaxed);
    if      (currentSrc == -2) inputSelector.setSelectedId (1, juce::dontSendNotification);
    else if (currentSrc == -1) inputSelector.setSelectedId (2, juce::dontSendNotification);
    else                       inputSelector.setSelectedId (100 + currentSrc, juce::dontSendNotification);

    auto styleCombo = [] (juce::ComboBox& c)
    {
        c.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff202024));
        c.setColour (juce::ComboBox::textColourId,       juce::Colour (0xffd0d0d0));
        c.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff404048));
    };
    styleCombo (inputSelector);
    inputSelector.onChange = [this] { onInputSelectorChanged(); };
    addAndMakeVisible (inputSelector);

    // ── Mode selector (Mono / Stereo / MIDI) ──
    modeSelector.addItem ("Mono",   1);   // ID 1 = Mode::Mono
    modeSelector.addItem ("Stereo", 2);   // ID 2 = Mode::Stereo
    modeSelector.addItem ("MIDI",   3);   // ID 3 = Mode::Midi
    modeSelector.setSelectedId (track.mode.load (std::memory_order_relaxed) + 1,
                                  juce::dontSendNotification);
    modeSelector.setTooltip ("Track signal mode. Mono = 1 audio input. "
                              "Stereo = 2 audio inputs (L + R) recorded as a "
                              "stereo WAV. MIDI = capture from a MIDI port "
                              "and feed the strip's hosted plugin.");
    styleCombo (modeSelector);
    modeSelector.onChange = [this] { onTrackModeChanged(); };
    addAndMakeVisible (modeSelector);

    // ── Stereo R-channel input (mirrors the L selector's options) ──
    inputSelectorR.addItem ("In " + juce::String (trackIndex + 2), 1);   // ID 1 = follow (-2 -> L+1)
    inputSelectorR.addItem ("None", 2);                                   // ID 2 = -1
    for (int i = 0; i < 16; ++i)
        inputSelectorR.addItem ("In " + juce::String (i + 1) + " (fixed)", 100 + i);
    {
        const int rSrc = track.inputSourceR.load (std::memory_order_relaxed);
        if      (rSrc == -2) inputSelectorR.setSelectedId (1, juce::dontSendNotification);
        else if (rSrc == -1) inputSelectorR.setSelectedId (2, juce::dontSendNotification);
        else                 inputSelectorR.setSelectedId (100 + rSrc, juce::dontSendNotification);
    }
    styleCombo (inputSelectorR);
    inputSelectorR.onChange = [this]
    {
        const int id = inputSelectorR.getSelectedId();
        int v = -2;
        if      (id == 1)              v = -2;
        else if (id == 2)              v = -1;
        else if (id >= 100 && id < 200) v = id - 100;
        track.inputSourceR.store (v, std::memory_order_relaxed);
    };
    addChildComponent (inputSelectorR);   // visibility toggled by refreshInputSelectorVisibility

    // ── MIDI input selector (placeholder until phase 3) ──
    midiInputSelector.addItem ("(MIDI input - phase 3)", 1);
    midiInputSelector.setEnabled (false);
    styleCombo (midiInputSelector);
    addChildComponent (midiInputSelector);

    refreshInputSelectorVisibility();

    // ── Aux send knobs (Mixing stage only) ──
    // Phase A: knobs + atomics only - audio routing through the aux buses
    // happens in Phase B. The four send levels feed AUX 1..4's plugin chain
    // (reverb / delay / etc.). Default -inf (no send).
    auxRowLabel.setText ("AUX", juce::dontSendNotification);
    auxRowLabel.setJustificationType (juce::Justification::centred);
    auxRowLabel.setColour (juce::Label::textColourId, juce::Colour (0xff9080c0));
    auxRowLabel.setFont (juce::Font (juce::FontOptions (8.5f, juce::Font::bold)));
    addChildComponent (auxRowLabel);

    static const juce::Colour kAuxColours[ChannelStripParams::kNumAuxSends] = {
        juce::Colour (0xff5a8ad0),    // AUX 1 - blue
        juce::Colour (0xff9080c0),    // AUX 2 - violet
        juce::Colour (0xffe0c050),    // AUX 3 - amber
        juce::Colour (0xff60c060),    // AUX 4 - green
    };

    auto formatAuxSend = [] (float dB)
    {
        if (dB <= ChannelStripParams::kAuxSendMinDb + 0.01f)
            return juce::String (juce::CharPointer_UTF8 ("\xe2\x88\x92"));   // "−" (U+2212)
        // Tight format: integer when |v| >= 10 ("-12"), else 1 decimal ("0.0").
        // Drops the "dB" suffix so the label fits the narrow column.
        if (std::abs (dB) >= 10.0f) return juce::String ((int) std::round (dB));
        return juce::String (dB, 1);
    };

    for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
    {
        auto knob = std::make_unique<juce::Slider> (
            juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox);
        knob->setRange (ChannelStripParams::kAuxSendMinDb, ChannelStripParams::kAuxSendMaxDb, 0.1);
        knob->setSkewFactorFromMidPoint (-12.0);   // detail near unity
        knob->setColour (juce::Slider::rotarySliderFillColourId,    kAuxColours[i]);
        knob->setColour (juce::Slider::rotarySliderOutlineColourId, juce::Colour (0xff404048));
        knob->setDoubleClickReturnValue (true, ChannelStripParams::kAuxSendOffDb);
        knob->setTooltip ("AUX " + juce::String (i + 1) + " send level. "
                          "Defaults post-fader; pre/post toggle coming in Phase D.");

        // Map "fully CCW" of the slider onto the kAuxSendOffDb sentinel so a
        // knob at minimum stops the audio path entirely (Phase B will read
        // this). Without the sentinel, even a "minimum-but-not-quite" send
        // would still tap audio, making muting via the knob impossible.
        const float initial = track.strip.auxSendDb[i].load (std::memory_order_relaxed);
        knob->setValue (initial <= ChannelStripParams::kAuxSendMinDb + 0.01f
                          ? ChannelStripParams::kAuxSendOffDb
                          : initial,
                          juce::dontSendNotification);

        auto* knobPtr = knob.get();
        const int idx = i;
        knob->onValueChange = [this, knobPtr, idx, formatAuxSend]
        {
            const float v = (float) knobPtr->getValue();
            const float stored = (v <= ChannelStripParams::kAuxSendMinDb + 0.01f)
                                    ? ChannelStripParams::kAuxSendOffDb : v;
            track.strip.auxSendDb[idx].store (stored, std::memory_order_relaxed);
            auxKnobLabels[(size_t) idx].setText (formatAuxSend (stored), juce::dontSendNotification);
        };
        knob->onDragStart = [this, idx]
        {
            track.strip.auxSendTouched[(size_t) idx].store (true, std::memory_order_release);
        };
        knob->onDragEnd = [this, idx]
        {
            track.strip.auxSendTouched[(size_t) idx].store (false, std::memory_order_release);
        };
        addChildComponent (knob.get());
        auxKnobs[(size_t) i] = std::move (knob);

        auxKnobLabels[(size_t) i].setJustificationType (juce::Justification::centred);
        auxKnobLabels[(size_t) i].setFont (juce::Font (juce::FontOptions (9.0f, juce::Font::bold)));
        auxKnobLabels[(size_t) i].setColour (juce::Label::textColourId, kAuxColours[i].brighter (0.2f));
        auxKnobLabels[(size_t) i].setText (formatAuxSend (initial), juce::dontSendNotification);
        addChildComponent (auxKnobLabels[(size_t) i]);
    }

    // Plugin slot button - left-click to load/replace via file chooser;
    // right-click to unload. Empty state shows "+ Plugin".
    pluginSlotButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff222226));
    pluginSlotButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff9080c0));
    pluginSlotButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xffd0c0e0));
    pluginSlotButton.setTooltip (juce::CharPointer_UTF8 (
        "Empty: click to pick a plugin from your system (VST3 / LV2 etc.). "
        "Loaded: click to toggle the plugin editor; right-click for "
        "Replace / Remove."));
    pluginSlotButton.onClick = [this]
    {
        if (! pluginSlot.isLoaded())
            openPluginPicker();
        else
            togglePluginEditor();
    };
    pluginSlotButton.onRightClick = [this] (const juce::MouseEvent&)
    {
        showPluginSlotMenu();
    };
    addAndMakeVisible (pluginSlotButton);

    // Compact-mode placeholder buttons. Hidden by default; setCompactMode(true)
    // makes them visible and hides the full inline EQ + COMP controls.
    styleCompactSectionButton (eqCompactButton,   juce::Colour (fourKColors::kLfGreen));
    styleCompactSectionButton (compCompactButton, juce::Colour (fourKColors::kCompGold));
    eqCompactButton  .setButtonText ("EQ");
    compCompactButton.setButtonText ("COMP");
    eqCompactButton  .setTooltip ("Click to open the EQ editor (click again to close). "
                                    "Button is illuminated when the section is enabled.");
    compCompactButton.setTooltip ("Click to open the compressor editor (click again to close). "
                                    "Button is illuminated when the section is enabled.");
    eqCompactButton  .onClick = [this] { openEqEditorPopup(); };
    compCompactButton.onClick = [this] { openCompEditorPopup(); };
    addChildComponent (eqCompactButton);    // hidden until compact mode flips on
    addChildComponent (compCompactButton);
}

ChannelStripComponent::~ChannelStripComponent()
{
    // If a popup editor is still on screen when the strip dies (e.g. the
    // window is closing), force-delete it so its content (which references
    // `track`) doesn't outlive us. Same SafePointer pattern as the audio
    // settings dialog in MainComponent.
    // CallOutBoxes auto-clean themselves when their content is deleted, but
    // we dismiss any in-flight box so it disappears immediately rather than
    // lingering through the next message-loop tick.
    if (auto* eq   = activeEqBox.getComponent())   eq->dismiss();
    if (auto* cmp  = activeCompBox.getComponent()) cmp->dismiss();
    if (activePluginEditorDialog != nullptr) delete activePluginEditorDialog.getComponent();
}

void ChannelStripComponent::setEqSectionVisible (bool visible)
{
    hpfKnob.setVisible (visible);
    hpfLabel.setVisible (visible);
    eqHeaderLabel.setVisible (visible);
    eqTypeButton.setVisible (visible);
    for (auto& row : eqRows)
    {
        if (row.gain) row.gain->setVisible (visible);
        if (row.freq) row.freq->setVisible (visible);
        if (row.q)    row.q   ->setVisible (visible);
        row.labelLeft .setVisible (visible);
        row.labelRight.setVisible (visible);
        row.rowLabel  .setVisible (visible);
        row.qLabel    .setVisible (visible);
    }
}

void ChannelStripComponent::setCompSectionVisible (bool visible)
{
    compOnButton .setVisible (visible);
    compModeOpto .setVisible (visible);
    compModeFet  .setVisible (visible);
    compModeVca  .setVisible (visible);
    optoPeakRedKnob .setVisible (visible);  optoPeakRedLabel.setVisible (visible);
    optoGainKnob    .setVisible (visible);  optoGainLabel   .setVisible (visible);
    optoLimitButton .setVisible (visible);
    fetInputKnob    .setVisible (visible);  fetInputLabel   .setVisible (visible);
    fetOutputKnob   .setVisible (visible);  fetOutputLabel  .setVisible (visible);
    fetAttackKnob   .setVisible (visible);  fetAttackLabel  .setVisible (visible);
    fetReleaseKnob  .setVisible (visible);  fetReleaseLabel .setVisible (visible);
    fetRatioKnob    .setVisible (visible);  fetRatioLabel   .setVisible (visible);
    vcaRatioKnob    .setVisible (visible);  vcaRatioLabel   .setVisible (visible);
    vcaAttackKnob   .setVisible (visible);  vcaAttackLabel  .setVisible (visible);
    vcaReleaseKnob  .setVisible (visible);  vcaReleaseLabel .setVisible (visible);
    vcaOutputKnob   .setVisible (visible);  vcaOutputLabel  .setVisible (visible);
    // compMeter now lives INSIDE the COMP section (next to the per-mode
    // knobs) so it follows section visibility. In compact mode the popup
    // owns its own threshold drag so we don't lose access.
    if (compMeter != nullptr) compMeter->setVisible (visible);
    threshMeterLabel.setVisible (false);   // legacy "THR" header, no longer used
    grPeakLabel    .setVisible (false);    // numeric GR readout retired - the
                                            // meter bar already shows GR clearly
}

void ChannelStripComponent::setCompactMode (bool compact)
{
    if (compactMode == compact) return;
    compactMode = compact;

    setEqSectionVisible   (! compact);
    setCompSectionVisible (! compact);
    eqCompactButton  .setVisible (compact);
    compCompactButton.setVisible (compact);

    resized();
    repaint();
}

void ChannelStripComponent::setMixingMode (bool mixing)
{
    if (mixingMode == mixing) return;
    mixingMode = mixing;

    // The mode/input/IN/ARM/PRINT block is hidden in Mixing - those controls
    // are tracking-stage only. The aux send knobs take that real estate.
    modeSelector     .setVisible (! mixing);
    inputSelector    .setVisible (! mixing);
    inputSelectorR   .setVisible (! mixing);
    midiInputSelector.setVisible (! mixing);
    monitorButton    .setVisible (! mixing);
    armButton        .setVisible (! mixing);
    printButton      .setVisible (! mixing);

    auxRowLabel.setVisible (mixing);
    for (auto& k : auxKnobs)        if (k != nullptr) k->setVisible (mixing);
    for (auto& l : auxKnobLabels)   l.setVisible (mixing);

    if (mixing)
        refreshInputSelectorVisibility();   // not needed since selectors are hidden,
                                            // but re-syncs when we leave Mixing later

    resized();
    repaint();
}

// Click semantics for the compact-mode placeholder buttons:
//   • Click while no popup is open → open this section's popup
//   • Click while THIS popup is already open → close it (toggle off)
//   • Click while the OTHER popup is open → close the other, open this one
// The mutual-exclusion path lets the user move quickly between EQ and COMP
// without first having to dismiss the open dialog manually.
void ChannelStripComponent::openPluginPicker()
{
    // PluginManager owns the KnownPluginList, populated by an explicit
    // user-triggered scan or by the cache loaded at startup.
    auto& mgr   = pluginSlot.getManagerForUi();
    auto& known = mgr.getKnownPluginList();

    juce::PopupMenu menu;
    if (known.getNumTypes() == 0)
    {
        menu.addSectionHeader ("No plugins scanned yet");
    }
    else
    {
        // KnownPluginList builds a hierarchical menu grouped by manufacturer.
        // IDs returned through this menu are 1-based indices into the list.
        // We start them at 1 and reserve 9000+ for our action items.
        known.addToMenu (menu,
                          juce::KnownPluginList::sortByManufacturer,
                          /*dirsToIgnore*/ {});
    }
    menu.addSeparator();
    menu.addItem (9001, "Scan plugins (VST3 / LV2)...");
    menu.addItem (9002, "Browse for file...");

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options()
                          .withTargetComponent (&pluginSlotButton),
                          [safe] (int result)
    {
        if (auto* self = safe.getComponent())
        {
            if (result == 0) return;  // cancelled
            if (result == 9001) { self->runPluginScanModal(); self->openPluginPicker(); return; }
            if (result == 9002) { self->openPluginFileChooser(); return; }

            // 1..N maps to KnownPluginList index (KnownPluginList::getIndexChosenByMenu
            // is the canonical decoder).
            auto& mgrRef = self->pluginSlot.getManagerForUi();
            const auto& known = mgrRef.getKnownPluginList();
            const int idx = known.getIndexChosenByMenu (result);
            if (idx < 0 || idx >= known.getNumTypes()) return;

            const auto* desc = known.getType (idx);
            if (desc == nullptr) return;

            juce::String error;
            if (! self->pluginSlot.loadFromDescription (*desc, error))
            {
                juce::AlertWindow::showAsync (
                    juce::MessageBoxOptions()
                        .withIconType (juce::MessageBoxIconType::WarningIcon)
                        .withTitle ("Plugin load failed")
                        .withMessage (error.isEmpty() ? "Unknown error" : error)
                        .withButton ("OK"),
                    nullptr);
            }
            self->refreshPluginSlotButton();
        }
    });
}

void ChannelStripComponent::runPluginScanModal()
{
    // Synchronous scan with a tiny modal banner so the user sees progress.
    // PluginDirectoryScanner is internally synchronous when allowAsync=false;
    // for a polished UX we'd lift it onto a thread with a real progress bar
    // - TODO once the picker is in active use.
    auto* dialog = new juce::AlertWindow ("Scanning plugins",
                                            "Looking through VST3 / LV2 install "
                                            "locations… this can take a few "
                                            "seconds the first time.",
                                            juce::MessageBoxIconType::NoIcon);
    dialog->setUsingNativeTitleBar (true);
    dialog->enterModalState (false /*not blocking*/);

    auto& mgr = pluginSlot.getManagerForUi();
    const int added = mgr.scanInstalledPlugins();

    delete dialog;

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::InfoIcon)
            .withTitle ("Plugin scan complete")
            .withMessage (juce::String::formatted (
                "Added %d plugin%s to the picker. (Total known: %d)",
                added, added == 1 ? "" : "s",
                mgr.getKnownPluginList().getNumTypes()))
            .withButton ("OK"),
        nullptr);
}

void ChannelStripComponent::openPluginFileChooser()
{
    // Linux VST3 plugins are directories (.vst3 bundles). The chooser uses
    // canSelectDirectories so the user can pick the bundle root. AsyncWork
    // shape: the chooser owns itself in `activePluginChooser` so it survives
    // the showAsync return, and we delete it on the callback.
    activePluginChooser = std::make_unique<juce::FileChooser> (
        "Select a plugin",
        juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".vst3"),
        "*.vst3;*.so;*.lv2");

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    activePluginChooser->launchAsync (
        juce::FileBrowserComponent::openMode |
        juce::FileBrowserComponent::canSelectFiles |
        juce::FileBrowserComponent::canSelectDirectories,
        [safe] (const juce::FileChooser& chooser)
        {
            if (auto* self = safe.getComponent())
            {
                const auto file = chooser.getResult();
                if (file == juce::File()) { self->activePluginChooser.reset(); return; }

                juce::String error;
                if (! self->pluginSlot.loadFromFile (file, error))
                {
                    juce::AlertWindow::showAsync (
                        juce::MessageBoxOptions()
                            .withIconType (juce::MessageBoxIconType::WarningIcon)
                            .withTitle ("Plugin load failed")
                            .withMessage (error)
                            .withButton ("OK"),
                        nullptr);
                }
                self->refreshPluginSlotButton();
                self->activePluginChooser.reset();
            }
        });
}

void ChannelStripComponent::unloadPluginSlot()
{
    // Close any open editor BEFORE the plugin is destroyed - the editor
    // holds a reference back to the AudioProcessor and would crash on
    // teardown otherwise.
    closePluginEditor();
    pluginSlot.unload();
    refreshPluginSlotButton();
}

void ChannelStripComponent::showPluginSlotMenu()
{
    juce::PopupMenu menu;
    if (pluginSlot.isLoaded())
    {
        // Editor toggle headline so right-click ALSO becomes a way to open
        // the plugin GUI (some users find right-click more discoverable).
        const bool editorOpen = (activePluginEditorDialog != nullptr);
        menu.addItem (2001, editorOpen ? "Close editor" : "Open editor");
        menu.addSeparator();
        menu.addItem (2002, "Replace plugin...");
        menu.addItem (2003, "Remove plugin");
        if (pluginSlot.wasAutoBypassed())
            menu.addItem (2004, "Re-enable plugin (auto-bypassed)");
    }
    else
    {
        menu.addItem (2010, "Pick plugin...");
    }

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&pluginSlotButton),
        [safe] (int result)
        {
            auto* self = safe.getComponent();
            if (self == nullptr || result <= 0) return;
            switch (result)
            {
                case 2001: self->togglePluginEditor();             break;
                case 2002: self->openPluginPicker();               break;
                case 2003: self->unloadPluginSlot();               break;
                case 2004: self->pluginSlot.clearAutoBypass();     break;
                case 2010: self->openPluginPicker();               break;
                default: break;
            }
        });
}

void ChannelStripComponent::refreshPluginSlotButton()
{
    const auto name = pluginSlot.getLoadedName();
    if (name == lastSlotName) return;
    lastSlotName = name;
    // Loaded buttons get a small "▾" prefix to telegraph that the button
    // is more than a single-action target - right-click reveals the
    // Replace / Remove menu.
    if (name.isNotEmpty())
        pluginSlotButton.setButtonText (juce::String (juce::CharPointer_UTF8 ("\xe2\x96\xbe ")) + name);
    else
        pluginSlotButton.setButtonText ("+ Plugin");

    // If the plugin was unloaded out from under an open editor (e.g. via
    // the right-click menu's Remove), the editor's dialog now references a
    // dead AudioProcessor - close it.
    if (name.isEmpty() && activePluginEditorDialog != nullptr)
        closePluginEditor();
}

void ChannelStripComponent::togglePluginEditor()
{
    // Toggle: if the editor is up, close it; otherwise open. Same shape as
    // the EQ / COMP popup buttons in compact mode.
    if (activePluginEditorDialog != nullptr)
        closePluginEditor();
    else
        openPluginEditor();
}

void ChannelStripComponent::openPluginEditor()
{
    if (activePluginEditorDialog != nullptr) return;  // already open

    auto* instance = pluginSlot.getInstance();
    if (instance == nullptr || ! instance->hasEditor()) return;

    auto* editor = instance->createEditorIfNeeded();
    if (editor == nullptr) return;

    // DialogWindow takes ownership; on dismissal the dialog deletes the
    // editor, and AudioProcessorEditor's destructor calls
    // editorBeingDeleted on the plugin instance - that's the
    // canonical lifecycle pattern.
    juce::DialogWindow::LaunchOptions opts;
    opts.content.setOwned (editor);
    opts.dialogTitle = pluginSlot.getLoadedName().isNotEmpty()
                          ? pluginSlot.getLoadedName()
                          : juce::String ("Plugin");
    opts.dialogBackgroundColour = juce::Colour (0xff181820);
    opts.escapeKeyTriggersCloseButton = true;
    opts.useNativeTitleBar = true;
    opts.resizable = editor->isResizable();
    activePluginEditorDialog = opts.launchAsync();
}

void ChannelStripComponent::closePluginEditor()
{
    if (auto* dlg = activePluginEditorDialog.getComponent())
        delete dlg;  // SafePointer auto-zeros
}

void ChannelStripComponent::openEqEditorPopup()
{
    // Toggle: clicking EQ while EQ is up dismisses it. With CallOutBox the
    // click that lands on the button while the box is open is consumed by
    // the box's dismissal handler (setDismissalMouseClicksAreAlwaysConsumed
    // below), so we never actually re-enter this function on the second
    // click - the box dismisses itself and the button never sees the click.
    // This branch handles programmatic toggle calls (e.g. keyboard shortcut).
    if (auto* existing = activeEqBox.getComponent())
    {
        existing->dismiss();
        return;
    }

    // Mutual exclusion: close the COMP popup if it's open.
    if (auto* otherBox = activeCompBox.getComponent())
        otherBox->dismiss();

    auto panel = std::make_unique<ChannelEqEditor> (track);
    panel->setSize (380, 628);

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr) topLevel = this;
    const auto anchor = topLevel->getLocalArea (this, eqCompactButton.getBounds());

    auto& box = juce::CallOutBox::launchAsynchronously (
        std::move (panel), anchor, topLevel);
    box.setDismissalMouseClicksAreAlwaysConsumed (true);
    // Force the popup to fire from the RIGHT side of the button by
    // restricting the available fit-area to the strip's right edge onward.
    // Without this, JUCE's CallOutBox picks whichever side has the most
    // space, which can drop below the button when the strip is centred.
    {
        const auto rightFit = topLevel->getLocalBounds()
                                  .withTrimmedLeft (anchor.getRight());
        box.updatePosition (anchor, rightFit);
    }
    activeEqBox = &box;
}

void ChannelStripComponent::openCompEditorPopup()
{
    if (auto* existing = activeCompBox.getComponent())
    {
        existing->dismiss();
        return;
    }
    if (auto* otherBox = activeEqBox.getComponent())
        otherBox->dismiss();

    auto panel = std::make_unique<ChannelCompEditor> (track);
    // Editor sets its own size based on the active comp mode (Opto is
    // shorter than FET/VCA). The CallOutBox follows via childBoundsChanged.

    auto* topLevel = getTopLevelComponent();
    if (topLevel == nullptr) topLevel = this;
    const auto anchor = topLevel->getLocalArea (this, compCompactButton.getBounds());

    auto& box = juce::CallOutBox::launchAsynchronously (
        std::move (panel), anchor, topLevel);
    box.setDismissalMouseClicksAreAlwaysConsumed (true);
    // Force right-side placement (same logic as the EQ popup).
    {
        const auto rightFit = topLevel->getLocalBounds()
                                  .withTrimmedLeft (anchor.getRight());
        box.updatePosition (anchor, rightFit);
    }
    activeCompBox = &box;
}

void ChannelStripComponent::timerCallback()
{
    // Plugin-slot button reflects the slot's current load state. Cheap -
    // just an atomic-pointer read + string compare against the cached name.
    refreshPluginSlotButton();

    // Sync the inline COMP on/off button with the underlying atom so it
    // reflects writes made from other surfaces - the meter-strip threshold
    // drag now auto-enables the comp, the popup editor's ON toggle, etc.
    {
        const bool engineCompOn = track.strip.compEnabled.load (std::memory_order_relaxed);
        if (compOnButton.getToggleState() != engineCompOn)
            compOnButton.setToggleState (engineCompOn, juce::dontSendNotification);
    }

    // SUMMARY-mode compact buttons get an on-air indicator so the user
    // knows whether EQ / COMP is engaged without having to open the
    // popup. Bullet character + brighter background when on.
    if (compactMode)
    {
        // Channel-strip EQ has no master on/off - it's "active" when the
        // HPF is engaged or any of the four bands has a non-trivial gain.
        // Comp has a real on/off atom.
        const auto& sp = track.strip;
        const bool anyBandActive =
               std::abs (sp.lfGainDb.load (std::memory_order_relaxed)) > 0.1f
            || std::abs (sp.lmGainDb.load (std::memory_order_relaxed)) > 0.1f
            || std::abs (sp.hmGainDb.load (std::memory_order_relaxed)) > 0.1f
            || std::abs (sp.hfGainDb.load (std::memory_order_relaxed)) > 0.1f;
        const bool eqOn   = sp.hpfEnabled.load (std::memory_order_relaxed) || anyBandActive;
        const bool compOn = sp.compEnabled.load (std::memory_order_relaxed);

        const auto eqAccent   = juce::Colour (fourKColors::kLfGreen);
        const auto compAccent = juce::Colour (fourKColors::kCompGold);

        // Text stays as the section name; the illuminated background
        // (set below) is the on-state indicator. No leading bullet/dot.
        if (eqCompactButton.getButtonText() != "EQ")
            eqCompactButton.setButtonText ("EQ");
        if (compCompactButton.getButtonText() != "COMP")
            compCompactButton.setButtonText ("COMP");

        eqCompactButton  .setColour (juce::TextButton::buttonColourId,
                                       eqOn   ? eqAccent.darker (0.55f)   : juce::Colour (0xff222226));
        compCompactButton.setColour (juce::TextButton::buttonColourId,
                                       compOn ? compAccent.darker (0.55f) : juce::Colour (0xff222226));
    }

    // Detect bus-colour edits made via the aux strip's right-click menu and
    // re-skin the bus assignment buttons accordingly. Polling at 30 Hz is
    // negligible (16 strips × 4 buses).
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
    {
        const auto current = session.bus (i).colour;
        if (current.getARGB() != lastBusColours[(size_t) i])
        {
            lastBusColours[(size_t) i] = current.getARGB();
            if (auto& btn = busButtons[(size_t) i])
            {
                btn->setColour (juce::TextButton::buttonOnColourId, current);
                btn->setColour (juce::TextButton::textColourOffId,  current.brighter (0.15f));
                btn->repaint();
            }
        }
    }

    const float gr = track.meterGrDb.load (std::memory_order_relaxed);
    if (gr < displayedGrDb)
        displayedGrDb = gr;
    else
        displayedGrDb += (gr - displayedGrDb) * 0.18f;

    // Input level meter - fast attack on rise, slow decay; with a peak-hold
    // marker that lingers for ~600 ms before falling. Stereo mode also
    // smooths the R channel so the second LED bar can be drawn alongside.
    auto smoothMeter = [] (float incoming, float& displayed,
                            float& peakHold, int& peakHoldFrames)
    {
        if (incoming > displayed) displayed = incoming;
        else                       displayed += (incoming - displayed) * 0.15f;

        if (incoming >= peakHold)
        {
            peakHold       = incoming;
            peakHoldFrames = 18;  // ~600 ms at 30 Hz
        }
        else if (peakHoldFrames > 0) --peakHoldFrames;
        else peakHold = juce::jmax (-100.0f, peakHold - 1.5f);
    };

    const float inputDb = track.meterInputDb.load (std::memory_order_relaxed);
    smoothMeter (inputDb, displayedInputDb, inputPeakHoldDb, inputPeakHoldFrames);

    const bool stereoMode = (track.mode.load (std::memory_order_relaxed)
                              == (int) Track::Mode::Stereo);
    if (stereoMode)
    {
        const float inputRDb = track.meterInputRDb.load (std::memory_order_relaxed);
        smoothMeter (inputRDb, displayedInputRDb, inputPeakHoldRDb, inputPeakHoldRFrames);
    }
    else
    {
        // Bleed the R channel back to silence when the user flips out of
        // stereo so the dual-bar doesn't ghost-hold its last value.
        displayedInputRDb = -100.0f;
        inputPeakHoldRDb  = -100.0f;
    }

    // GR repaint is handled inside CompMeterStrip's own Timer.
    if (! inputMeterArea.isEmpty())  repaint (inputMeterArea);

    // Update the numeric readout below the meter.
    if (inputPeakHoldDb <= -60.0f)
        inputPeakLabel.setText ("-inf", juce::dontSendNotification);
    else
        inputPeakLabel.setText (juce::String (inputPeakHoldDb, 1), juce::dontSendNotification);

    // Tint the readout when peaks get hot.
    inputPeakLabel.setColour (juce::Label::textColourId,
        inputPeakHoldDb >= -3.0f  ? juce::Colour (0xffff5050) :
        inputPeakHoldDb >= -12.0f ? juce::Colour (0xffe0c050) :
                                     juce::Colour (0xffd0d0d0));

    // GR readout: show "-X.X" when the comp is reducing, dim "0.0" otherwise.
    // displayedGrDb is already smoothed above (asymmetric: fast attack on
    // rise, slow release on fall), matching the visual GR meter.
    if (displayedGrDb <= -0.05f)
    {
        grPeakLabel.setText (juce::String (displayedGrDb, 1), juce::dontSendNotification);
        grPeakLabel.setColour (juce::Label::textColourId, juce::Colour (0xffe0c050));
    }
    else
    {
        grPeakLabel.setText ("0.0", juce::dontSendNotification);
        grPeakLabel.setColour (juce::Label::textColourId, juce::Colour (0xff606064));
    }

    // Motor-fader / motor-pan animation: when the audio engine is feeding
    // a live atom from the lane (Read, or Touch when not grabbed), mirror
    // it into the slider/knob visually. Gate on a small delta to avoid
    // setValue churn every tick when manual mode just mirrors the user's
    // setpoint. While the user is grabbing in Touch mode, their gesture
    // IS the value source, so we don't fight it.
    {
        const int amode = track.automationMode.load (std::memory_order_relaxed);
        const bool isRead  = amode == (int) AutomationMode::Read;
        const bool isWrite = amode == (int) AutomationMode::Write;
        const bool isTouch = amode == (int) AutomationMode::Touch;
        const bool playing = engine.getTransport().isPlaying();

        // Fader animate / capture.
        {
            const float live    = track.strip.liveFaderDb.load (std::memory_order_relaxed);
            const bool  touched = track.strip.faderTouched.load (std::memory_order_relaxed);
            const bool  animating = isRead || (isTouch && ! touched);
            if (animating && std::abs (live - displayedLiveFaderDb) > 0.05f)
            {
                displayedLiveFaderDb = live;
                faderSlider.setValue (live, juce::dontSendNotification);
            }
            else if (! animating)
            {
                displayedLiveFaderDb = live;
            }

            const bool capturing = playing && (isWrite || (isTouch && touched));
            if (capturing)
                captureWritePoint (AutomationParam::FaderDb,
                                    track.strip.faderDb.load (std::memory_order_relaxed));
        }

        // Pan animate / capture. Same shape as fader; threshold is in
        // pan units (-1..+1), so 0.005 = 0.25 % of the knob's travel,
        // well below visible.
        {
            const float live    = track.strip.livePan.load (std::memory_order_relaxed);
            const bool  touched = track.strip.panTouched.load (std::memory_order_relaxed);
            const bool  animating = isRead || (isTouch && ! touched);
            if (animating && std::abs (live - displayedLivePan) > 0.005f)
            {
                displayedLivePan = live;
                panKnob.setValue (live, juce::dontSendNotification);
            }
            else if (! animating)
            {
                displayedLivePan = live;
            }

            const bool capturing = playing && (isWrite || (isTouch && touched));
            if (capturing)
                captureWritePoint (AutomationParam::Pan,
                                    track.strip.pan.load (std::memory_order_relaxed));
        }

        // Aux sends - animate + capture in lockstep with fader / pan.
        // Threshold 0.1 dB on a -60..+6 dB knob (~0.15 % of travel).
        // Knob is null when the strip isn't in mixing mode (visible).
        for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
        {
            const float live    = track.strip.liveAuxSendDb[(size_t) i].load (std::memory_order_relaxed);
            const bool  touched = track.strip.auxSendTouched[(size_t) i].load (std::memory_order_relaxed);
            const bool  animating = isRead || (isTouch && ! touched);
            if (animating && std::abs (live - displayedLiveAuxSendDb[(size_t) i]) > 0.1f)
            {
                displayedLiveAuxSendDb[(size_t) i] = live;
                if (auto* knob = auxKnobs[(size_t) i].get())
                    knob->setValue (live, juce::dontSendNotification);
            }
            else if (! animating)
            {
                displayedLiveAuxSendDb[(size_t) i] = live;
            }

            const bool capturing = playing && (isWrite || (isTouch && touched));
            if (capturing)
            {
                const auto param = (AutomationParam) ((int) AutomationParam::AuxSend1 + i);
                captureWritePoint (param,
                                    track.strip.auxSendDb[(size_t) i].load (std::memory_order_relaxed));
            }
        }
    }
}

void ChannelStripComponent::captureWritePoint (AutomationParam param, float denormValue)
{
    // Convert denormalized value back to lane storage (0..1). Mirrors
    // Session.cpp's denormalizeAutomation - kept here as a small switch
    // because there are only two callers (this and a future Touch hook)
    // and a free function isn't worth the noise.
    auto normalize = [] (AutomationParam p, float v) -> float
    {
        switch (p)
        {
            case AutomationParam::FaderDb:
            {
                const float lo = ChannelStripParams::kFaderMinDb;
                const float hi = ChannelStripParams::kFaderMaxDb;
                return juce::jlimit (0.0f, 1.0f, (v - lo) / (hi - lo));
            }
            case AutomationParam::Pan:
                return juce::jlimit (0.0f, 1.0f, (v + 1.0f) * 0.5f);
            case AutomationParam::Mute:
            case AutomationParam::Solo:
                return v >= 0.5f ? 1.0f : 0.0f;
            case AutomationParam::AuxSend1:
            case AutomationParam::AuxSend2:
            case AutomationParam::AuxSend3:
            case AutomationParam::AuxSend4:
            {
                if (v <= ChannelStripParams::kAuxSendOffDb) return 0.0f;
                const float lo = ChannelStripParams::kAuxSendMinDb;
                const float hi = ChannelStripParams::kAuxSendMaxDb;
                return juce::jlimit (0.0f, 1.0f, (v - lo) / (hi - lo));
            }
            case AutomationParam::kCount: break;
        }
        return 0.0f;
    };

    auto& lane = track.automationLanes[(size_t) param];
    AutomationPoint pt;
    pt.timeSamples   = engine.getTransport().getPlayhead();
    pt.value         = normalize (param, denormValue);
    pt.recordedAtBPM = session.tempoBpm.load (std::memory_order_relaxed);

    // Coalesce: if the most recent point is at the same timeline sample
    // (or earlier), replace its value (don't append). This handles two
    // cases: (a) Timer fires faster than transport advances (paused?
    // shouldn't happen since we gated on isPlaying), (b) Time-travel
    // backward via loop wraparound mid-Write -- subsequent appends
    // belong AFTER the most recent timeline position, not before it.
    // Strict ascending invariant is required by evaluateLane's binary
    // search.
    if (! lane.points.empty() && lane.points.back().timeSamples >= pt.timeSamples)
    {
        // Loop wraparound case: drop the rest of the lane that's now in
        // the future relative to playhead, so the binary-search invariant
        // (sorted ascending) holds and the upcoming Write captures land
        // in their natural order. Discrete params (mute / solo) keep
        // the same rule.
        if (lane.points.back().timeSamples > pt.timeSamples)
        {
            auto cutoff = std::lower_bound (lane.points.begin(), lane.points.end(),
                pt.timeSamples,
                [] (const AutomationPoint& a, juce::int64 t) { return a.timeSamples < t; });
            lane.points.erase (cutoff, lane.points.end());
        }
        // Same-sample replace: keep the latest value at this exact sample.
        if (! lane.points.empty() && lane.points.back().timeSamples == pt.timeSamples)
        {
            lane.points.back() = pt;
            return;
        }
    }
    lane.points.push_back (pt);
}

void ChannelStripComponent::onAutoModeClicked()
{
    // 3c-ii cycle: OFF -> READ -> WRITE -> TOUCH -> OFF.
    const int cur = track.automationMode.load (std::memory_order_relaxed);
    int next = (int) AutomationMode::Off;
    switch ((AutomationMode) cur)
    {
        case AutomationMode::Off:   next = (int) AutomationMode::Read;  break;
        case AutomationMode::Read:  next = (int) AutomationMode::Write; break;
        case AutomationMode::Write: next = (int) AutomationMode::Touch; break;
        case AutomationMode::Touch: next = (int) AutomationMode::Off;   break;
    }

    // When transitioning OUT of Write or Touch, the points just appended
    // to the lane need to be visible to the audio thread BEFORE it starts
    // reading from the lane. The release-store on mode synchronizes those
    // writes - any prior append to lane.points happens-before the audio
    // thread's acquire-load of the new mode.
    track.automationMode.store (next, std::memory_order_release);

    // Read mode disables every automated control (spec: "User cannot
    // override"). Off / Write / Touch leave them interactive so the
    // user can either ride them (Write) or grab to override (Touch).
    const bool interactive = next != (int) AutomationMode::Read;
    faderSlider.setEnabled (interactive);
    panKnob    .setEnabled (interactive);
    for (auto& knob : auxKnobs)
        if (knob != nullptr) knob->setEnabled (interactive);

    refreshAutoModeButton();
}

void ChannelStripComponent::refreshAutoModeButton()
{
    const int m = track.automationMode.load (std::memory_order_relaxed);
    const char* label = "OFF";
    juce::Colour bg (0xff222226);
    juce::Colour fg (0xff909094);
    switch (m)
    {
        case (int) AutomationMode::Read:
            label = "READ";
            bg = juce::Colour (0xff20603a);   // muted green
            fg = juce::Colour (0xffd0e8d0);
            break;
        case (int) AutomationMode::Write:
            label = "WRITE";
            bg = juce::Colour (0xff803020);   // muted red - 3c-ii
            fg = juce::Colour (0xfff0d0c8);
            break;
        case (int) AutomationMode::Touch:
            label = "TOUCH";
            bg = juce::Colour (0xff806020);   // muted amber - 3c-ii
            fg = juce::Colour (0xfff0e0c0);
            break;
        case (int) AutomationMode::Off:
        default: break;
    }
    autoModeButton.setButtonText (label);
    autoModeButton.setColour (juce::TextButton::buttonColourId, bg);
    autoModeButton.setColour (juce::TextButton::textColourOffId, fg);
}

void ChannelStripComponent::mouseDown (const juce::MouseEvent& e)
{
    // Right-click anywhere on the strip body opens the colour menu. Children
    // (sliders, buttons, labels) consume their own mouse events first, so this
    // only fires on background pixels - exactly the affordance we want.
    if (e.mods.isPopupMenu())
        showColourMenu();
    else
        Component::mouseDown (e);
}

void ChannelStripComponent::applyTrackColour (juce::Colour c)
{
    track.colour = c;
    repaint();
}

void ChannelStripComponent::showColourMenu()
{
    // Eight 4K-palette presets for fast picking, plus a "Custom…" entry that
    // pops a JUCE ColourSelector for fine-grained choice.
    const std::pair<const char*, juce::uint32> presets[] = {
        { "Red",        fourKColors::kHfRed     },
        { "Orange",     fourKColors::kHmOrange  },
        { "Amber",      fourKColors::kLmAmber   },
        { "Green",      fourKColors::kLfGreen   },
        { "Cyan",       fourKColors::kPanCyan   },
        { "Blue",       fourKColors::kHpfBlue   },
        { "Purple",     fourKColors::kSendPurple},
        { "Tan",        fourKColors::kMasterTan },
    };

    juce::PopupMenu menu;
    menu.addSectionHeader ("Track colour");
    for (size_t i = 0; i < std::size (presets); ++i)
    {
        juce::PopupMenu::Item item;
        item.itemID = (int) (i + 1);
        item.text = presets[i].first;
        item.colour = juce::Colour (presets[i].second);
        menu.addItem (item);
    }
    menu.addSeparator();
    menu.addItem (1001, "Rename track...");

    // Plugin slot menu items, only meaningful when a plugin is loaded.
    // Replace/Remove live here (rather than on the slot button itself) so
    // the slot button's primary click stays as a toggle for the editor.
    if (pluginSlot.isLoaded())
    {
        menu.addSeparator();
        menu.addItem (1010, "Replace plugin...");
        menu.addItem (1011, "Remove plugin");
        if (pluginSlot.wasAutoBypassed())
            menu.addItem (1012, "Re-enable plugin (auto-bypassed)");
    }

    juce::Component::SafePointer<ChannelStripComponent> safe (this);
    // Copy the presets into a std::vector so the async callback owns its own
    // copy (the local C-array on the stack is gone by the time the menu fires).
    std::vector<std::pair<juce::String, juce::uint32>> presetCopy;
    presetCopy.reserve (std::size (presets));
    for (auto& p : presets) presetCopy.emplace_back (juce::String (p.first), p.second);

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
        [safe, presetCopy] (int result)
        {
            if (result <= 0) return;
            auto* self = safe.getComponent();
            if (self == nullptr) return;
            if (result == 1001)
            {
                self->nameLabel.showEditor();
                return;
            }
            if (result == 1010) { self->openPluginPicker();      return; }
            if (result == 1011) { self->unloadPluginSlot();      return; }
            if (result == 1012) { self->pluginSlot.clearAutoBypass(); return; }
            const int idx = result - 1;
            if (idx >= 0 && idx < (int) presetCopy.size())
                self->applyTrackColour (juce::Colour (presetCopy[(size_t) idx].second));
        });
}

void ChannelStripComponent::onInputSelectorChanged()
{
    const int id = inputSelector.getSelectedId();
    int src = -2;  // follow track index
    if      (id == 1)            src = -2;
    else if (id == 2)            src = -1;
    else if (id >= 100)          src = id - 100;
    track.inputSource.store (src, std::memory_order_relaxed);
}

void ChannelStripComponent::onTrackModeChanged()
{
    // ComboBox IDs: 1 = Mono, 2 = Stereo, 3 = MIDI. Stored as int on the
    // Track so the audio thread can read it lock-free.
    const int id = modeSelector.getSelectedId();
    const int mode = juce::jlimit (0, 2, id - 1);  // 0..2 = Track::Mode
    track.mode.store (mode, std::memory_order_relaxed);
    refreshInputSelectorVisibility();
    // Resize so the layout reflects the new mode (extra dropdown for stereo,
    // hidden audio dropdown for MIDI).
    resized();
    repaint();
}

void ChannelStripComponent::refreshInputSelectorVisibility()
{
    const int mode = juce::jlimit (0, 2, track.mode.load (std::memory_order_relaxed));
    const bool isMono   = (mode == 0);
    const bool isStereo = (mode == 1);
    const bool isMidi   = (mode == 2);

    inputSelector    .setVisible (isMono || isStereo);
    inputSelectorR   .setVisible (isStereo);
    midiInputSelector.setVisible (isMidi);
}

void ChannelStripComponent::onHpfKnobChanged()
{
    const float freq = (float) hpfKnob.getValue();
    track.strip.hpfFreq.store (freq, std::memory_order_relaxed);
    // Bypass the HPF DSP entirely when the knob is at the floor - saves
    // 16 channels worth of biquad cost when nobody's using HPF.
    track.strip.hpfEnabled.store (freq > ChannelStripParams::kHpfOffHz + 0.5f,
                                   std::memory_order_relaxed);
}

void ChannelStripComponent::refreshEqTypeButton()
{
    eqTypeButton.setButtonText (eqTypeButton.getToggleState() ? "G" : "E");
}

void ChannelStripComponent::setCompMode (int modeIndex)
{
    track.strip.compMode.store (modeIndex, std::memory_order_relaxed);
    refreshCompModeButtons();
    resized();  // mode swap reshapes the comp body
}

void ChannelStripComponent::refreshCompModeButtons()
{
    const int m = juce::jlimit (0, 2, track.strip.compMode.load (std::memory_order_relaxed));
    compModeOpto.setToggleState (m == 0, juce::dontSendNotification);
    compModeFet.setToggleState  (m == 1, juce::dontSendNotification);
    compModeVca.setToggleState  (m == 2, juce::dontSendNotification);

    // Show only the active mode's controls.
    const bool isOpto = (m == 0), isFet = (m == 1), isVca = (m == 2);
    optoPeakRedKnob.setVisible (isOpto);  optoPeakRedLabel.setVisible (isOpto);
    optoGainKnob   .setVisible (isOpto);  optoGainLabel   .setVisible (isOpto);
    optoLimitButton.setVisible (isOpto);

    fetInputKnob   .setVisible (isFet);   fetInputLabel   .setVisible (isFet);
    fetOutputKnob  .setVisible (isFet);   fetOutputLabel  .setVisible (isFet);
    fetAttackKnob  .setVisible (isFet);   fetAttackLabel  .setVisible (isFet);
    fetReleaseKnob .setVisible (isFet);   fetReleaseLabel .setVisible (isFet);
    fetRatioKnob   .setVisible (isFet);   fetRatioLabel   .setVisible (isFet);

    vcaRatioKnob   .setVisible (isVca);   vcaRatioLabel   .setVisible (isVca);
    vcaAttackKnob  .setVisible (isVca);   vcaAttackLabel  .setVisible (isVca);
    vcaReleaseKnob .setVisible (isVca);   vcaReleaseLabel .setVisible (isVca);
    vcaOutputKnob  .setVisible (isVca);   vcaOutputLabel  .setVisible (isVca);
}

static void drawSectionPlaceholder (juce::Graphics& g, juce::Rectangle<int> r,
                                    const juce::String& label, juce::Colour accent)
{
    if (r.isEmpty()) return;
    g.setColour (juce::Colour (0xff222226));
    g.fillRoundedRectangle (r.toFloat(), 3.0f);
    g.setColour (accent.withAlpha (0.45f));
    g.drawRoundedRectangle (r.toFloat().reduced (0.5f), 3.0f, 0.8f);
    g.setColour (accent.withAlpha (0.85f));
    g.setFont (juce::Font (juce::FontOptions (9.5f, juce::Font::bold)));
    g.drawText (label, r.reduced (4, 2), juce::Justification::centredTop, false);
}

void ChannelStripComponent::paint (juce::Graphics& g)
{
    auto r = getLocalBounds().toFloat().reduced (1.5f);
    g.setColour (juce::Colour (0xff1a1a1c));
    g.fillRoundedRectangle (r, 4.0f);
    g.setColour (track.colour.withAlpha (0.85f));
    g.fillRoundedRectangle (r.removeFromTop (4.0f), 2.0f);
    g.setColour (juce::Colour (0xff2a2a2e));
    g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (1.5f), 4.0f, 1.0f);

    // EQ region: full background (header + HPF row + 4 band rows)
    if (! eqArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff1f231e));
        g.fillRoundedRectangle (eqArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (0xff80c090).withAlpha (0.40f));
        g.drawRoundedRectangle (eqArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    if (! compArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff241f1c));
        g.fillRoundedRectangle (compArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (fourKColors::kCompGold).withAlpha (0.40f));
        g.drawRoundedRectangle (compArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    // SEND box (Mixing stage only) - same framed-block shape as EQ/COMP
    // with the AUX purple accent so the row reads as a coherent section
    // instead of floating loose knobs above PAN.
    if (! auxRowArea.isEmpty())
    {
        g.setColour (juce::Colour (0xff1f1d24));
        g.fillRoundedRectangle (auxRowArea.toFloat(), 3.0f);
        g.setColour (juce::Colour (0xff9080c0).withAlpha (0.40f));
        g.drawRoundedRectangle (auxRowArea.toFloat().reduced (0.5f), 3.0f, 0.8f);
    }

    // ── Channel input LED (next to the fader). Shows the pre-fader signal
    //    level so the engineer always sees what's hitting the strip.
    //    Threshold + GR meters live INSIDE the COMP section now, so this is
    //    just a clean input bar with a peak-hold tick.
    constexpr float kFloorDb   = -60.0f;
    constexpr float kCeilingDb =   0.0f;
    auto dbToFrac = [] (float db) {
        return juce::jlimit (0.0f, 1.0f, (db - kFloorDb) / (kCeilingDb - kFloorDb));
    };

    if (! inputMeterArea.isEmpty())
    {
        const bool stereoMode = (track.mode.load (std::memory_order_relaxed)
                                  == (int) Track::Mode::Stereo);

        // Single bar (mono / midi) or split L|R bars (stereo) in the same
        // total meter footprint. Stereo halves the bar width so both fit
        // without expanding the meter column.
        auto drawBar = [&] (juce::Rectangle<float> bar, float dispDb, float peakDb)
        {
            g.setColour (juce::Colour (0xff060608));
            g.fillRoundedRectangle (bar, 1.5f);
            g.setColour (juce::Colour (0xff2a2a30));
            g.drawRoundedRectangle (bar, 1.5f, 0.5f);

            const float frac = dbToFrac (dispDb);
            if (frac > 0.001f)
            {
                const float fillH = (bar.getHeight() - 2.0f) * frac;
                const float x = bar.getX() + 1.0f;
                const float w = bar.getWidth() - 2.0f;
                const float y = bar.getBottom() - 1.0f - fillH;
                juce::ColourGradient grad (juce::Colour (0xffe05050), x, bar.getY(),
                                             juce::Colour (0xff44d058), x, bar.getBottom(),
                                             false);
                grad.addColour (dbToFrac (-12.0f), juce::Colour (0xffe0c050));
                grad.addColour (dbToFrac  (-3.0f), juce::Colour (0xffd07040));
                g.setGradientFill (grad);
                g.fillRect (juce::Rectangle<float> (x, y, w, fillH));
            }

            const float peakFrac = dbToFrac (peakDb);
            if (peakFrac > 0.001f)
            {
                const float y = bar.getBottom() - 1.0f - peakFrac * (bar.getHeight() - 2.0f);
                g.setColour (peakFrac > dbToFrac (-3.0f) ? juce::Colour (0xffff8080)
                                                          : juce::Colour (0xfff0f0f0));
                g.fillRect (juce::Rectangle<float> (bar.getX() + 1.0f, y - 0.5f,
                                                      bar.getWidth() - 2.0f, 1.4f));
            }

            const int segments = juce::jlimit (8, 30, (int) (bar.getHeight() / 3.5f));
            const float segStep = bar.getHeight() / (float) segments;
            g.setColour (juce::Colour (0xff020203));
            for (int i = 1; i < segments; ++i)
            {
                const float yy = bar.getY() + i * segStep;
                g.fillRect (juce::Rectangle<float> (bar.getX() + 1.0f, yy - 0.4f,
                                                      bar.getWidth() - 2.0f, 0.8f));
            }
        };

        if (stereoMode)
        {
            const auto full = inputMeterArea.toFloat();
            const float halfW = (full.getWidth() - 1.0f) * 0.5f;   // 1 px gap between bars
            const auto barL = juce::Rectangle<float> (full.getX(),               full.getY(),
                                                        halfW, full.getHeight());
            const auto barR = juce::Rectangle<float> (full.getX() + halfW + 1.0f, full.getY(),
                                                        halfW, full.getHeight());
            drawBar (barL, displayedInputDb,  inputPeakHoldDb);
            drawBar (barR, displayedInputRDb, inputPeakHoldRDb);

            // Tiny "L" / "R" caption above each bar so the user immediately
            // sees the meter is now stereo.
            g.setColour (juce::Colour (0xffa0a0a8));
            g.setFont (juce::Font (juce::FontOptions (7.5f, juce::Font::bold)));
            g.drawText ("L", juce::Rectangle<float> (barL.getX(), barL.getY() - 9.0f,
                                                       barL.getWidth(), 8.0f),
                          juce::Justification::centred, false);
            g.drawText ("R", juce::Rectangle<float> (barR.getX(), barR.getY() - 9.0f,
                                                       barR.getWidth(), 8.0f),
                          juce::Justification::centred, false);
        }
        else
        {
            drawBar (inputMeterArea.toFloat(), displayedInputDb, inputPeakHoldDb);
        }
    }

    // Fader dB scale - labels in the scale column aligned with the tick
    // marks the LookAndFeel paints across the fader's track. Same set of
    // values as kFaderTicks; format matches the screenshot's absolute-
    // value style ("6", "3", "0", "3", "6", "12", "24", "40", "90").
    if (! meterScaleArea.isEmpty())
    {
        const auto scale = meterScaleArea;
        const auto& range = faderSlider.getNormalisableRange();
        for (const auto& t : kFaderTicks)
        {
            if (t.db < range.start - 0.01f || t.db > range.end + 0.01f) continue;
            const float y = faderYForDb (faderSlider, t.db);
            const bool isZero = (std::abs (t.db) < 0.01f);
            g.setColour (isZero ? juce::Colour (0xffe0e0e0) : juce::Colour (0xff909094));
            g.setFont (juce::Font (juce::FontOptions (isZero ? 9.5f : 8.5f,
                                                        isZero ? juce::Font::bold
                                                                : juce::Font::plain)));
            const auto rect = juce::Rectangle<float> ((float) scale.getX(), y - 5.0f,
                                                        (float) scale.getWidth(), 10.0f);
            g.drawText (t.label, rect, juce::Justification::centredRight, false);
        }
    }
}

void ChannelStripComponent::resized()
{
    auto area = getLocalBounds().reduced (4);
    area.removeFromTop (6);

    nameLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (3);

    // Mixing stage swaps the tracking-only block (mode + input + IN/ARM/PRINT)
    // for a row of 4 AUX send knobs. Recording/Mastering keep the original
    // block. setMixingMode() drives this; here we just lay out whatever's
    // currently visible.
    // Tracking-only block (mode selector + input + IN/ARM/PRINT). Hidden in
    // Mixing stage - the AUX send knobs land below the COMP section instead
    // (signal-flow placement: EQ → COMP → SENDS → PAN → fader).
    if (! mixingMode)
    {
        modeSelector.setBounds (area.removeFromTop (18));
        area.removeFromTop (2);

        const int mode = juce::jlimit (0, 2, track.mode.load (std::memory_order_relaxed));
        auto inputRow = area.removeFromTop (18);
        if (mode == 0)
        {
            inputSelector.setBounds (inputRow);
        }
        else if (mode == 1)
        {
            const int half = inputRow.getWidth() / 2;
            inputSelector .setBounds (inputRow.removeFromLeft (half).withTrimmedRight (1));
            inputSelectorR.setBounds (inputRow.withTrimmedLeft (1));
        }
        else  // MIDI
        {
            midiInputSelector.setBounds (inputRow);
        }

        area.removeFromTop (3);
        auto buttonRow = area.removeFromTop (18);
        const int colW = buttonRow.getWidth() / 3;
        monitorButton.setBounds (buttonRow.removeFromLeft (colW).reduced (1));
        armButton    .setBounds (buttonRow.removeFromLeft (colW).reduced (1));
        printButton  .setBounds (buttonRow.reduced (1));
        area.removeFromTop (2);
    }

    // Plugin-slot button right below the IN/ARM/PRINT row. Always visible -
    // available in both compact and full modes since it's independent of the
    // EQ/COMP collapse.
    pluginSlotButton.setBounds (area.removeFromTop (20).reduced (2, 0));
    area.removeFromTop (3);

    // In compact mode the EQ + COMP sections collapse to two narrow buttons
    // so the fader / bus assigns / meter / M-S-Ø stay visible when the
    // SUMMARY view is consuming half the window. Click either button to
    // open the full editor as a popup.
    if (compactMode)
    {
        eqCompactButton  .setBounds (area.removeFromTop (24).reduced (2, 0));
        area.removeFromTop (4);
        compCompactButton.setBounds (area.removeFromTop (24).reduced (2, 0));
        area.removeFromTop (8);
        eqArea   = juce::Rectangle<int>();
        compArea = juce::Rectangle<int>();
    }
    else
    {

    // ── EQ block ─ SSL 9000J / E-EQ inspired layout.
    //   • Each band is a 2-column pair: GAIN on the left, FREQ on the right
    //     (same Y, prominent same-size knobs).
    //   • Bell bands (HM, LM) add a smaller Q knob STACKED BELOW the gain in
    //     the same left column - no third column competing for horizontal
    //     space, freq stays at full size on the right.
    //   • HPF lives at the top of the block as a single centred knob.
    //   • Shelf rows are short; bell rows are taller (gain block + Q block).
    // All primary knobs share the pan-knob diameter (26 px) - frees vertical
    // space for the fader and keeps the strip readable at 16-track widths.
    // Q stays smaller (20 px) since it's a subordinate control on bell bands.
    constexpr int kKnobSize    = 26;
    constexpr int kValueLabelH = 14;
    constexpr int kKnobBlockH  = kKnobSize + kValueLabelH + 2;        // 42
    constexpr int kQKnobSize   = 26;                                  // Q matches gain/freq per user directive
    constexpr int kQBlockH     = kQKnobSize + kValueLabelH;           // 40
    constexpr int kFreqYStagger = 2;                                  // tighter SSL nudge - was 4
    constexpr int kEqHeaderH   = 12;  // tighter - pulls HPF closer to the EQ label
    constexpr int kEqHpfRowH   = kKnobBlockH;                         // 42 - no extra padding
    constexpr int kEqShelfRowH = kKnobBlockH + kFreqYStagger + 2;     // 46 - tighter than before
    constexpr int kEqBellRowH  = kKnobBlockH + kQBlockH;              // 82

    // 1 HPF row + 2 shelf rows (HF, LF) + 2 bell rows (HM, LM). No vertical
    // reduce - every pixel of EQ height is used so the knobs hug the header.
    eqArea = area.removeFromTop (kEqHeaderH + kEqHpfRowH + 2 * kEqShelfRowH + 2 * kEqBellRowH);
    {
        auto s = eqArea.reduced (3, 0);
        auto header = s.removeFromTop (kEqHeaderH);
        eqHeaderLabel.setBounds (header.removeFromLeft (header.getWidth() - 26));
        eqTypeButton.setBounds  (header.reduced (1, 1));

        constexpr int kRowLabelW = 28;

        // HPF row - single centred knob, hugging the top of the EQ block.
        {
            auto row = s.removeFromTop (kEqHpfRowH);
            hpfLabel.setBounds (row.removeFromLeft (kRowLabelW));
            const int knobBoundsW = juce::jmin (60, row.getWidth());
            const int hpfX = row.getX() + (row.getWidth() - knobBoundsW) / 2;
            hpfKnob.setBounds (hpfX, row.getY(), knobBoundsW, kKnobBlockH);
        }

        // Band rows. Knobs are top-aligned within each row (no leading
        // padding) - keeps everything pulled up toward the EQ header.
        for (size_t i = 0; i < eqRows.size(); ++i)
        {
            const bool hasQ = eqRows[i].q != nullptr;
            const int rowH = hasQ ? kEqBellRowH : kEqShelfRowH;
            auto row = s.removeFromTop (rowH);
            // Row-label area, vertically aligned to the rotary's centre so
            // "HM" / "LM" / etc sit next to the actual knob, not centred
            // within the row's full height (which puts them next to the
            // value text instead). The label box is sized to the rotary
            // area only (kKnobSize) - JUCE's centred Label vertical-centres
            // the text within those bounds.
            auto labelArea = row.removeFromLeft (kRowLabelW);
            const int rotaryH = kKnobSize;  // height of the actual rotary
            eqRows[i].rowLabel.setBounds (labelArea.getX(), row.getY(),
                                           labelArea.getWidth(), rotaryH);
            if (hasQ)
            {
                const int qRotaryH = kQKnobSize;
                const int qY       = row.getY() + kKnobBlockH;
                eqRows[i].qLabel.setBounds (labelArea.getX(), qY,
                                             labelArea.getWidth(), qRotaryH);
            }

            const int colW = row.getWidth() / 2;
            const int leftX  = row.getX();
            const int rightX = row.getX() + colW;
            const int gainY = row.getY();
            // Bell rows have a Q knob stacked under the gain on the left;
            // the freq sits in its own column on the right and is now
            // vertically centred inside the FULL bell row (between the
            // gain and Q rows). Shelf rows keep the small SSL nudge.
            const int freqY = hasQ
                ? row.getY() + (rowH - kKnobBlockH) / 2
                : gainY + kFreqYStagger;

            eqRows[i].gain->setBounds (leftX,  gainY, colW, kKnobBlockH);
            eqRows[i].freq->setBounds (rightX, freqY, colW, kKnobBlockH);

            if (hasQ)
            {
                const int qW    = juce::jmin (colW, 44);
                const int qX    = leftX + (colW - qW) / 2;
                const int qY    = gainY + kKnobBlockH;
                eqRows[i].q->setBounds (qX, qY, qW, kQBlockH);
            }
        }
    }
    area.removeFromTop (3);

    // COMP region:
    //   Header  : ON button
    //   Mode    : O / F / V
    //   Body    : per-mode knob set on the LEFT, threshold/IN/GR meter on
    //             the RIGHT. Putting the meter inside the comp section
    //             (rather than next to the fader) keeps all comp UI grouped
    //             and frees up the fader column for a taller fader.
    constexpr int kCompKnobSize     = 26;
    constexpr int kCompKnobBlockH   = kCompKnobSize + kValueLabelH + 2;
    constexpr int kCompKnobLabelH   = 10;
    constexpr int kCompKnobRowH     = kCompKnobLabelH + kCompKnobBlockH;  // ~52
    constexpr int kCompKnobGap      = 4;
    constexpr int kCompMeterW       = 26;   // CompMeterStrip handle + 2 bars
    constexpr int kCompMeterGap     = 4;

    // Every mode now fits in 2 rows - Opto pairs PEAK+GAIN with LIMIT shrunk
    // to a small toggle on row 2; FET squeezes IN/OUT/RATIO across row 1 with
    // ATK/REL on row 2; VCA stays at RAT/OUT + ATK/REL. Body height is fixed
    // so the strip layout doesn't shift when the user switches modes.
    constexpr int kCompBodyH = 2 * kCompKnobRowH + kCompKnobGap;
    compArea = area.removeFromTop (16 + 2 + 16 + 4 + kCompBodyH + 4);
    {
        auto s = compArea.reduced (3, 2);

        compOnButton.setBounds (s.removeFromTop (16));
        s.removeFromTop (2);

        auto modeRow = s.removeFromTop (16);
        const int modeColW = modeRow.getWidth() / 3;
        compModeOpto.setBounds (modeRow.removeFromLeft (modeColW).reduced (1, 0));
        compModeFet.setBounds  (modeRow.removeFromLeft (modeColW).reduced (1, 0));
        compModeVca.setBounds  (modeRow.reduced (1, 0));
        s.removeFromTop (4);

        // Body: split into knob area (left) + meter strip (right). Meter
        // shows pre-comp input level, GR, and the threshold drag handle.
        auto body = s.removeFromTop (kCompBodyH);
        auto meterRect = body.removeFromRight (kCompMeterW);
        body.removeFromRight (kCompMeterGap);
        if (compMeter != nullptr)
            compMeter->setBounds (meterRect);

        auto layoutKnobCell = [] (juce::Rectangle<int> cell,
                                   juce::Slider& knob, juce::Label& label)
        {
            label.setBounds (cell.removeFromTop (kCompKnobLabelH));
            knob.setBounds  (cell.getX(), cell.getY(), cell.getWidth(), kCompKnobBlockH);
        };

        const int currentMode = juce::jlimit (0, 2,
            track.strip.compMode.load (std::memory_order_relaxed));

        if (currentMode == 0)  // Opto: PEAK + GAIN on row 1, LIMIT toggle on row 2
        {
            auto row1 = body.removeFromTop (kCompKnobRowH);
            body.removeFromTop (kCompKnobGap);
            auto row2 = body.removeFromTop (kCompKnobRowH);
            const int colW = row1.getWidth() / 2;
            layoutKnobCell (row1.removeFromLeft (colW), optoPeakRedKnob, optoPeakRedLabel);
            layoutKnobCell (row1,                       optoGainKnob,    optoGainLabel);
            const int limW = juce::jmin (54, row2.getWidth());
            optoLimitButton.setBounds (row2.getX() + (row2.getWidth() - limW) / 2,
                                        row2.getY() + (row2.getHeight() - 18) / 2,
                                        limW, 18);
        }
        else if (currentMode == 1)  // FET: IN/OUT/RATIO row 1, ATK/REL row 2
        {
            auto row1 = body.removeFromTop (kCompKnobRowH);
            body.removeFromTop (kCompKnobGap);
            auto row2 = body.removeFromTop (kCompKnobRowH);
            const int col3W = row1.getWidth() / 3;
            layoutKnobCell (row1.removeFromLeft (col3W), fetInputKnob,   fetInputLabel);
            layoutKnobCell (row1.removeFromLeft (col3W), fetRatioKnob,   fetRatioLabel);
            layoutKnobCell (row1,                        fetOutputKnob,  fetOutputLabel);
            const int col2W = row2.getWidth() / 2;
            layoutKnobCell (row2.removeFromLeft (col2W), fetAttackKnob,  fetAttackLabel);
            layoutKnobCell (row2,                        fetReleaseKnob, fetReleaseLabel);
        }
        else  // VCA: 4 knobs in 2×2 (threshold lives on the meter-strip handle)
        {
            auto row1 = body.removeFromTop (kCompKnobRowH);
            body.removeFromTop (kCompKnobGap);
            auto row2 = body.removeFromTop (kCompKnobRowH);
            const int colW = row1.getWidth() / 2;
            layoutKnobCell (row1.removeFromLeft (colW), vcaRatioKnob,   vcaRatioLabel);
            layoutKnobCell (row1,                       vcaOutputKnob,  vcaOutputLabel);
            layoutKnobCell (row2.removeFromLeft (colW), vcaAttackKnob,  vcaAttackLabel);
            layoutKnobCell (row2,                       vcaReleaseKnob, vcaReleaseLabel);
        }
    }
    area.removeFromTop (3);
    }   // end of else (! compactMode)

    // ── AUX sends (Mixing stage only). Single row of 4 knobs with a slight
    //    vertical zig-zag - even-index knobs sit higher, odd-index sit lower.
    //    Staggering keeps each knob full-size while they share a narrow strip
    //    width that wouldn't allow 4 knobs at the same Y without crowding the
    //    value labels. Sits between COMP and PAN to match signal-flow order:
    //    EQ → COMP → SENDS → PAN → fader.
    if (mixingMode)
    {
        constexpr int kAuxKnobSize  = 24;
        constexpr int kAuxStaggerY  = 10;     // odd knobs offset down by this much
        constexpr int kAuxValueH    = 10;
        constexpr int kAuxBlockH    = kAuxStaggerY + kAuxKnobSize + 2 + kAuxValueH;
        constexpr int kAuxLabelH    = 11;
        constexpr int kAuxLabelGap  = 1;
        constexpr int kAuxRowTotalH = kAuxLabelH + kAuxLabelGap + kAuxBlockH;

        // Capture the framed-box bounds for paint() (drawn in the same
        // style as eqArea / compArea, with the SEND purple accent).
        auxRowArea = area.removeFromTop (kAuxRowTotalH);

        // Lay out the label + knob block inside the box, with a small
        // horizontal padding so the SEND knobs don't kiss the frame.
        auto inner = auxRowArea.reduced (3, 0);
        auxRowLabel.setBounds (inner.removeFromTop (kAuxLabelH));
        inner.removeFromTop (kAuxLabelGap);

        auto block = inner.removeFromTop (kAuxBlockH);
        const int colW = block.getWidth() / ChannelStripParams::kNumAuxSends;

        for (int i = 0; i < ChannelStripParams::kNumAuxSends; ++i)
        {
            auto col = juce::Rectangle<int> (block.getX() + i * colW, block.getY(),
                                                colW, block.getHeight());
            const int knobX = col.getX() + (col.getWidth() - kAuxKnobSize) / 2;
            const int knobY = col.getY() + ((i % 2 == 0) ? 0 : kAuxStaggerY);
            if (auxKnobs[(size_t) i] != nullptr)
                auxKnobs[(size_t) i]->setBounds (knobX, knobY,
                                                  kAuxKnobSize, kAuxKnobSize);

            // Labels share the same Y at the bottom of the block, regardless
            // of the knob's vertical offset - keeps them aligned for easy
            // scanning even though the knobs zig-zag.
            const int labelY = col.getBottom() - kAuxValueH;
            auxKnobLabels[(size_t) i].setBounds (col.getX(), labelY,
                                                   col.getWidth(), kAuxValueH);
        }

        area.removeFromTop (3);
    }
    else
    {
        // Non-mixing stages don't show the SEND row, so the framed box
        // disappears with it.
        auxRowArea = juce::Rectangle<int>();
    }

    // The horizontal BUSES region used to live here. Bus toggles now sit in
    // a vertical column to the left of the fader (laid out below).

    {
        // Pan section: small "PAN" header above a compact red knob.
        constexpr int kPanKnobSize = 26;
        constexpr int kPanBlockH = kPanKnobSize + 12 + 2;
        constexpr int kPanLabelH = 11;
        auto panArea = area.removeFromTop (kPanLabelH + kPanBlockH + 2);
        panLabel.setBounds (panArea.removeFromTop (kPanLabelH));
        panKnob.setBounds (panArea.getX(), panArea.getY(), panArea.getWidth(), kPanBlockH);
    }
    area.removeFromTop (2);

    auto buttons = area.removeFromBottom (24);
    const int btnW = buttons.getWidth() / 3;
    muteButton .setBounds (buttons.removeFromLeft (btnW).reduced (1));
    soloButton .setBounds (buttons.removeFromLeft (btnW).reduced (1));
    phaseButton.setBounds (buttons.reduced (1));
    area.removeFromBottom (2);

    // Automation mode button - sits as a thin full-width row directly above
    // M/S/Ø. Single label that mirrors the current mode; click cycles.
    auto autoRow = area.removeFromBottom (16);
    autoModeButton.setBounds (autoRow.reduced (1, 0));
    area.removeFromBottom (4);

    // Fader + input meter pinned to the bottom of the strip. To the right of
    // the fader: a small dB scale column (0/-12/-24/-60) and a vertical LED
    // input meter. GR + threshold drag moved into the COMP section, so this
    // column is now slim and the fader gets the reclaimed width.
    constexpr int kMaxFaderHeight  = 360;  // 280 -> 360: bank row left ConsoleView, freed height goes to faders
    constexpr int kPeakLabelH      = 18;
    // Meter column width is mode-aware: in stereo we draw two bars side by
    // side, so we need extra room. Mono / Midi use a narrower column.
    const bool stereoMode = (track.mode.load (std::memory_order_relaxed)
                              == (int) Track::Mode::Stereo);
    const int kMeterWidth = stereoMode ? 18 : 12;
    constexpr int kMeterScaleWidth = 16;
    constexpr int kMeterGap        = 3;
    const int     kPeakColumnW     = kMeterScaleWidth + kMeterGap + kMeterWidth;

    auto faderArea = area;
    if (faderArea.getHeight() > kMaxFaderHeight + kPeakLabelH + 2)
        faderArea = faderArea.removeFromBottom (kMaxFaderHeight + kPeakLabelH + 2);

    // Vertical bus-assign column on the LEFT of the fader, mirroring the
    // meter column on the right. Tascam Model 2400 layout - the bus toggles
    // sit alongside the fader where the engineer's fingers naturally rest.
    constexpr int kBusColumnW   = 18;  // narrower - saves horizontal space for the fader
    constexpr int kBusColumnGap = 3;
    constexpr int kBusButtonH   = 20;  // shorter - sits alongside the fader without stealing height
    constexpr int kBusButtonGap = 2;
    auto busColumn = faderArea.removeFromLeft (kBusColumnW);
    faderArea.removeFromLeft (kBusColumnGap);

    auto meterColumn = faderArea.removeFromRight (kMeterWidth);
    faderArea.removeFromRight (kMeterGap);
    auto scaleColumn = faderArea.removeFromRight (kMeterScaleWidth);
    faderArea.removeFromRight (kMeterGap);

    // Position the 4 bus buttons vertically centred within busColumn.
    {
        const int totalButtonsH = ChannelStripParams::kNumBuses * kBusButtonH
                                + (ChannelStripParams::kNumBuses - 1) * kBusButtonGap;
        int y = busColumn.getY() + (busColumn.getHeight() - totalButtonsH) / 2;
        for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
        {
            busButtons[(size_t) i]->setBounds (busColumn.getX(), y, kBusColumnW, kBusButtonH);
            y += kBusButtonH + kBusButtonGap;
        }
    }

    // Peak readout beneath the meter column. GR readout retired - the GR bar
    // inside the COMP section is the canonical readout now.
    const int peakRight = meterColumn.getRight();
    const int peakLeft  = peakRight - kPeakColumnW;
    auto peakArea = juce::Rectangle<int> (peakLeft, meterColumn.getBottom() - kPeakLabelH,
                                           kPeakColumnW, kPeakLabelH);
    inputPeakLabel.setBounds (peakArea);
    grPeakLabel  .setVisible (false);

    meterColumn = meterColumn.withTrimmedBottom (kPeakLabelH + 2);
    scaleColumn = scaleColumn.withTrimmedBottom (kPeakLabelH + 2);

    // Hide the legacy "THR" header - threshold drag now lives in the COMP
    // section's meter strip, so the header next to the fader is misleading.
    threshMeterLabel.setVisible (false);

    inputMeterArea = meterColumn;
    meterScaleArea = scaleColumn;
    faderSlider.setBounds (faderArea);
}
} // namespace focal
