#include "ChannelStripComponent.h"
#include "ADHDawLookAndFeel.h"

namespace adhdaw
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

// Top-to-bottom order: HF (treble) on top, LF (bass) on the bottom — matches
// the SSL/console convention and the user's spatial expectation. HF and LF
// are shelf-only here (no Q), HM and LM are bell with Q exposed — three
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

ChannelStripComponent::ChannelStripComponent (int idx, Track& t, Session& s)
    : trackIndex (idx), track (t), session (s)
{
    nameLabel.setText (track.name, juce::dontSendNotification);
    nameLabel.setJustificationType (juce::Justification::centred);
    nameLabel.setColour (juce::Label::textColourId, juce::Colours::white);
    nameLabel.setFont (juce::Font (juce::FontOptions (13.0f, juce::Font::bold)));
    nameLabel.setEditable (false, true, false);  // single-click no, double-click YES, no submit-on-empty
    nameLabel.setTooltip ("Double-click to rename · right-click for colour");
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
                   0.0, 100.0, 50.0, 0.0, "Output gain (% — 50 = unity)",
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

    // FET ratio: 5-step knob (4:1 / 8:1 / 12:1 / 20:1 / All) — replaces the
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

    // ── VCA knobs (ratio/attack/release/output — threshold lives on the
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

        // Q knob (mid-bands only — bell-only HM/LM). Same size and styling
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

    // ── Bus assigns — on/off toggles laid out vertically alongside the fader.
    // Each button takes the colour of its corresponding aux bus header (the
    // same HSV ramp Session::Session() uses to colour the four aux strips),
    // so the engineer reads "BUS 1 = teal, BUS 2 = blue, …" without consulting
    // the strip headers. ──
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
    {
        const auto busColour = session.aux (i).colour;
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
    addAndMakeVisible (panKnob);

    faderSlider.setRange (ChannelStripParams::kFaderMinDb, ChannelStripParams::kFaderMaxDb, 0.1);
    faderSlider.setSkewFactorFromMidPoint (-12.0);
    faderSlider.setValue (track.strip.faderDb.load (std::memory_order_relaxed), juce::dontSendNotification);
    faderSlider.setDoubleClickReturnValue (true, 0.0);
    // No "dB" suffix — strip is narrow enough that "0.0 dB" truncates.
    // The dB scale column to the right of the meter makes the unit obvious.
    faderSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 50, 16);
    faderSlider.setTooltip ("Channel fader (dB) — double-click to reset to 0 dB");
    faderSlider.onValueChange = [this]
    {
        track.strip.faderDb.store ((float) faderSlider.getValue(), std::memory_order_relaxed);
    };
    addAndMakeVisible (faderSlider);

    muteButton.setClickingTogglesState (true);
    muteButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::orangered);
    muteButton.setToggleState (track.strip.mute.load (std::memory_order_relaxed), juce::dontSendNotification);
    muteButton.setTooltip ("Mute (M) — silences this channel at the master");
    muteButton.onClick = [this]
    {
        track.strip.mute.store (muteButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (muteButton);

    soloButton.setClickingTogglesState (true);
    soloButton.setColour (juce::TextButton::buttonOnColourId, juce::Colours::yellow.darker (0.2f));
    soloButton.setToggleState (track.strip.solo.load (std::memory_order_relaxed), juce::dontSendNotification);
    soloButton.setTooltip ("Solo (S) — when any channel is soloed, only soloed channels are heard");
    soloButton.onClick = [this]
    {
        track.strip.solo.store (soloButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (soloButton);

    phaseButton.setClickingTogglesState (true);
    phaseButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xff70c0d0));
    phaseButton.setToggleState (track.strip.phaseInvert.load (std::memory_order_relaxed),
                                 juce::dontSendNotification);
    phaseButton.setTooltip ("Polarity invert (Ø) — flips the input signal's polarity");
    phaseButton.onClick = [this]
    {
        track.strip.phaseInvert.store (phaseButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (phaseButton);

    // ── Peak input level readout ──
    inputPeakLabel.setJustificationType (juce::Justification::centred);
    inputPeakLabel.setColour (juce::Label::textColourId, juce::Colour (0xffd0d0d0));
    inputPeakLabel.setColour (juce::Label::backgroundColourId, juce::Colour (0xff0a0a0c));
    inputPeakLabel.setColour (juce::Label::outlineColourId,    juce::Colour (0xff2a2a2e));
    inputPeakLabel.setFont (juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                            13.0f, juce::Font::bold)));
    inputPeakLabel.setText ("-inf", juce::dontSendNotification);
    addAndMakeVisible (inputPeakLabel);

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
    monitorButton.setTooltip ("Input monitor — when on, live input is heard at the master. "
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
    armButton.setTooltip ("Record arm — press REC on the transport to capture this input");
    armButton.onClick = [this]
    {
        track.recordArmed.store (armButton.getToggleState(), std::memory_order_relaxed);
    };
    addAndMakeVisible (armButton);

    // PRINT toggle — when on, recording captures the post-EQ/post-comp signal
    // so effects are committed to the WAV. Off (default) = clean input on
    // disk so the engineer can re-EQ / re-comp at mix time.
    printButton.setClickingTogglesState (true);
    printButton.setColour (juce::TextButton::buttonColourId,   juce::Colour (0xff202024));
    printButton.setColour (juce::TextButton::buttonOnColourId, juce::Colour (0xffd09060));
    printButton.setColour (juce::TextButton::textColourOffId,  juce::Colour (0xff8a7060));
    printButton.setColour (juce::TextButton::textColourOnId,   juce::Colour (0xff121214));
    printButton.setToggleState (track.printEffects.load (std::memory_order_relaxed),
                                 juce::dontSendNotification);
    printButton.setTooltip ("PRINT — when on, the recorded WAV captures the post-EQ/post-comp signal "
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

    inputSelector.setColour (juce::ComboBox::backgroundColourId, juce::Colour (0xff202024));
    inputSelector.setColour (juce::ComboBox::textColourId,       juce::Colour (0xffd0d0d0));
    inputSelector.setColour (juce::ComboBox::outlineColourId,    juce::Colour (0xff404048));
    inputSelector.onChange = [this] { onInputSelectorChanged(); };
    addAndMakeVisible (inputSelector);
}

ChannelStripComponent::~ChannelStripComponent() = default;

void ChannelStripComponent::timerCallback()
{
    // Detect bus-colour edits made via the aux strip's right-click menu and
    // re-skin the bus assignment buttons accordingly. Polling at 30 Hz is
    // negligible (16 strips × 4 buses).
    for (int i = 0; i < ChannelStripParams::kNumBuses; ++i)
    {
        const auto current = session.aux (i).colour;
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

    // Input level meter — fast attack on rise, slow decay; with a peak-hold
    // marker that lingers for ~600 ms before falling.
    const float inputDb = track.meterInputDb.load (std::memory_order_relaxed);
    if (inputDb > displayedInputDb)
        displayedInputDb = inputDb;
    else
        displayedInputDb += (inputDb - displayedInputDb) * 0.15f;

    if (inputDb >= inputPeakHoldDb)
    {
        inputPeakHoldDb = inputDb;
        inputPeakHoldFrames = 18;  // ~600 ms at 30 Hz
    }
    else if (inputPeakHoldFrames > 0)
    {
        --inputPeakHoldFrames;
    }
    else
    {
        inputPeakHoldDb = juce::jmax (-100.0f, inputPeakHoldDb - 1.5f);
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
}

void ChannelStripComponent::mouseDown (const juce::MouseEvent& e)
{
    // Right-click anywhere on the strip body opens the colour menu. Children
    // (sliders, buttons, labels) consume their own mouse events first, so this
    // only fires on background pixels — exactly the affordance we want.
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

void ChannelStripComponent::onHpfKnobChanged()
{
    const float freq = (float) hpfKnob.getValue();
    track.strip.hpfFreq.store (freq, std::memory_order_relaxed);
    // Bypass the HPF DSP entirely when the knob is at the floor — saves
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

    // The channel input-level LED meter used to live here as a separate
    // vertical bar — it duplicated CompMeterStrip's input bar (both pulled
    // from the same `meterInputDb` atomic). The bar is now drawn ONCE by
    // CompMeterStrip, which sits in this position at the bottom of the strip
    // and hosts the threshold drag-handle + GR bar alongside it.

    // dB scale numbers next to the meter (0 / 12 / 24 / 60). Aligned to the
    // CompMeterStrip child's bounds since it now owns the actual input bar.
    if (! meterScaleArea.isEmpty() && compMeter != nullptr)
    {
        const auto bar = compMeter->getBounds().toFloat();
        constexpr float kFloorDb   = -60.0f;
        constexpr float kCeilingDb =   0.0f;
        auto dbToY = [&] (float db)
        {
            const float frac = juce::jlimit (0.0f, 1.0f, (db - kFloorDb) / (kCeilingDb - kFloorDb));
            return bar.getBottom() - 1.0f - frac * (bar.getHeight() - 2.0f);
        };

        g.setColour (juce::Colour (0xff707074));
        g.setFont (juce::Font (juce::FontOptions (8.5f)));
        const auto scale = meterScaleArea;
        for (auto entry : { std::pair<float, const char*> { 0.0f, "0" },
                            std::pair<float, const char*> { -12.0f, "12" },
                            std::pair<float, const char*> { -24.0f, "24" },
                            std::pair<float, const char*> { -48.0f, "48" } })
        {
            const float y = dbToY (entry.first);
            const auto rect = juce::Rectangle<float> ((float) scale.getX(), y - 5.0f,
                                                        (float) scale.getWidth(), 10.0f);
            g.drawText (entry.second, rect, juce::Justification::centredRight, false);
        }
    }

    // GR is now drawn inside the CompMeterStrip child component, not here.
}

void ChannelStripComponent::resized()
{
    auto area = getLocalBounds().reduced (4);
    area.removeFromTop (6);

    nameLabel.setBounds (area.removeFromTop (20));
    area.removeFromTop (3);

    // Input selector spans the full width, then a row of IN / ARM buttons.
    {
        inputSelector.setBounds (area.removeFromTop (18));
        area.removeFromTop (3);
        auto buttonRow = area.removeFromTop (18);
        const int colW = buttonRow.getWidth() / 3;
        monitorButton.setBounds (buttonRow.removeFromLeft (colW).reduced (1));
        armButton    .setBounds (buttonRow.removeFromLeft (colW).reduced (1));
        printButton  .setBounds (buttonRow.reduced (1));
    }
    area.removeFromTop (2);

    // ── EQ block ─ SSL 9000J / E-EQ inspired layout.
    //   • Each band is a 2-column pair: GAIN on the left, FREQ on the right
    //     (same Y, prominent same-size knobs).
    //   • Bell bands (HM, LM) add a smaller Q knob STACKED BELOW the gain in
    //     the same left column — no third column competing for horizontal
    //     space, freq stays at full size on the right.
    //   • HPF lives at the top of the block as a single centred knob.
    //   • Shelf rows are short; bell rows are taller (gain block + Q block).
    // All primary knobs share the pan-knob diameter (26 px) — frees vertical
    // space for the fader and keeps the strip readable at 16-track widths.
    // Q stays smaller (20 px) since it's a subordinate control on bell bands.
    constexpr int kKnobSize    = 26;
    constexpr int kValueLabelH = 14;
    constexpr int kKnobBlockH  = kKnobSize + kValueLabelH + 2;        // 42
    constexpr int kQKnobSize   = 26;                                  // Q matches gain/freq per user directive
    constexpr int kQBlockH     = kQKnobSize + kValueLabelH;           // 40
    constexpr int kFreqYStagger = 2;                                  // tighter SSL nudge — was 4
    constexpr int kEqHeaderH   = 12;  // tighter — pulls HPF closer to the EQ label
    constexpr int kEqHpfRowH   = kKnobBlockH;                         // 42 — no extra padding
    constexpr int kEqShelfRowH = kKnobBlockH + kFreqYStagger + 2;     // 46 — tighter than before
    constexpr int kEqBellRowH  = kKnobBlockH + kQBlockH;              // 82

    // 1 HPF row + 2 shelf rows (HF, LF) + 2 bell rows (HM, LM). No vertical
    // reduce — every pixel of EQ height is used so the knobs hug the header.
    eqArea = area.removeFromTop (kEqHeaderH + kEqHpfRowH + 2 * kEqShelfRowH + 2 * kEqBellRowH);
    {
        auto s = eqArea.reduced (3, 0);
        auto header = s.removeFromTop (kEqHeaderH);
        eqHeaderLabel.setBounds (header.removeFromLeft (header.getWidth() - 26));
        eqTypeButton.setBounds  (header.reduced (1, 1));

        constexpr int kRowLabelW = 28;

        // HPF row — single centred knob, hugging the top of the EQ block.
        {
            auto row = s.removeFromTop (kEqHpfRowH);
            hpfLabel.setBounds (row.removeFromLeft (kRowLabelW));
            const int knobBoundsW = juce::jmin (60, row.getWidth());
            const int hpfX = row.getX() + (row.getWidth() - knobBoundsW) / 2;
            hpfKnob.setBounds (hpfX, row.getY(), knobBoundsW, kKnobBlockH);
        }

        // Band rows. Knobs are top-aligned within each row (no leading
        // padding) — keeps everything pulled up toward the EQ header.
        for (size_t i = 0; i < eqRows.size(); ++i)
        {
            const bool hasQ = eqRows[i].q != nullptr;
            const int rowH = hasQ ? kEqBellRowH : kEqShelfRowH;
            auto row = s.removeFromTop (rowH);
            // Row-label area, vertically aligned to the rotary's centre so
            // "HM" / "LM" / etc sit next to the actual knob, not centred
            // within the row's full height (which puts them next to the
            // value text instead). The label box is sized to the rotary
            // area only (kKnobSize) — JUCE's centred Label vertical-centres
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
            const int freqY = gainY + kFreqYStagger;  // tiny SSL nudge — gain top-left, freq just below

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
    //   Body    : per-mode knob set fills the full width — the input/GR
    //             meter and threshold drag handle moved out of this section
    //             and now live alongside the channel level meter at the
    //             bottom of the strip (see CompMeterStrip placement below).
    constexpr int kCompKnobSize     = 26;
    constexpr int kCompKnobBlockH   = kCompKnobSize + kValueLabelH + 2;
    constexpr int kCompKnobLabelH   = 10;
    constexpr int kCompKnobRowH     = kCompKnobLabelH + kCompKnobBlockH;  // ~52
    constexpr int kCompKnobGap      = 4;

    // Every mode now fits in 2 rows — Opto pairs PEAK+GAIN with LIMIT shrunk
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

        // Body: mode-specific control area uses the full width. The meter
        // and threshold drag-handle moved to the bottom of the strip.
        auto body = s.removeFromTop (kCompBodyH);

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
    area.removeFromBottom (4);

    // Fader + input meter pinned to the bottom of the strip. To the right of
    // the fader: a small dB scale column (0/-12/-24/-60), a vertical LED
    // input meter, and a numeric peak readout below them. The peak readout
    // gets a wider column so a 5-char dBFS reading like "-36.5" fits cleanly.
    constexpr int kMaxFaderHeight  = 240;  // raised from 180 — saved EQ/COMP space goes to fader throw
    constexpr int kPeakLabelH      = 18;
    // CompMeterStrip's intrinsic layout: handle 10 + gap 1 + input 10 + gap 1
    // + GR 10 = 32 px. Replaces the old standalone LED meter (was 12 px wide)
    // — that 20-px increase is the cost of folding three meters into one.
    constexpr int kMeterWidth      = 32;
    constexpr int kMeterScaleWidth = 18;
    constexpr int kMeterGap        = 3;
    constexpr int kPeakColumnW     = kMeterScaleWidth + kMeterGap + kMeterWidth + 16;

    auto faderArea = area;
    if (faderArea.getHeight() > kMaxFaderHeight + kPeakLabelH + 2)
        faderArea = faderArea.removeFromBottom (kMaxFaderHeight + kPeakLabelH + 2);

    // Vertical bus-assign column on the LEFT of the fader, mirroring the
    // meter column on the right. Tascam Model 2400 layout — the bus toggles
    // sit alongside the fader where the engineer's fingers naturally rest.
    constexpr int kBusColumnW   = 18;  // narrower — saves horizontal space for the fader
    constexpr int kBusColumnGap = 3;
    constexpr int kBusButtonH   = 20;  // shorter — sits alongside the fader without stealing height
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

    // Peak readout spans the meter+scale columns plus a few pixels of overflow
    // into the fader column on the left, so 5-character readings fit.
    const int peakRight = meterColumn.getRight();
    const int peakLeft  = peakRight - kPeakColumnW;
    auto peakArea = juce::Rectangle<int> (peakLeft, meterColumn.getBottom() - kPeakLabelH,
                                           kPeakColumnW, kPeakLabelH);
    inputPeakLabel.setBounds (peakArea);

    meterColumn = meterColumn.withTrimmedBottom (kPeakLabelH + 2);
    scaleColumn = scaleColumn.withTrimmedBottom (kPeakLabelH + 2);

    // "THR" label header — sits above the meter+scale columns to label what
    // CompMeterStrip's drag handle controls. Reserves ~12 px at the top.
    constexpr int kThreshLabelH = 12;
    auto threshHeader = meterColumn.removeFromTop (kThreshLabelH);
    scaleColumn.removeFromTop (kThreshLabelH);  // keep the scale aligned with the bar
    threshMeterLabel.setBounds (threshHeader.expanded (8, 0));  // overflow a bit so "THR" fits

    // CompMeterStrip now occupies the meter column at the bottom of the
    // strip — it owns the input bar (replacing the old standalone LED meter),
    // the GR bar, and the threshold drag handle.
    if (compMeter != nullptr)
        compMeter->setBounds (meterColumn);
    inputMeterArea = {};            // unused — CompMeterStrip draws the input bar
    meterScaleArea = scaleColumn;
    faderSlider.setBounds (faderArea);
}
} // namespace adhdaw
