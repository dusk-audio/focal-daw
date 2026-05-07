#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <memory>
#include "CompMeterStrip.h"
#include "../session/Session.h"

namespace focal
{
class AudioEngine;

class ChannelStripComponent final : public juce::Component, private juce::Timer
{
public:
    ChannelStripComponent (int trackIndex, Track& trackRef, Session& sessionRef,
                            class PluginSlot& slotRef, AudioEngine& engineRef);
    ~ChannelStripComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent& e) override;

    // Compact mode hides the inline EQ + COMP controls and replaces each
    // section with a single small button that opens the corresponding
    // editor as a modal popup. Used when the SUMMARY (tape-strip) view is
    // expanded, so the fader / bus assigns / meters / M-S-Ø stay visible
    // even on a screen the SUMMARY ate half of.
    void setCompactMode (bool compact);
    bool isCompactMode() const noexcept { return compactMode; }

    // Mixing-stage flag - swaps the input/IN/ARM/PRINT block at the top of
    // the strip for a row of 4 AUX send knobs (one per aux bus). The aux
    // strips host reverb / delay / etc. plugin chains; these knobs are how
    // each channel feeds them.
    void setMixingMode (bool mixing);
    bool isMixingMode() const noexcept { return mixingMode; }

private:
    void timerCallback() override;

    int trackIndex;
    Track& track;
    Session& session;
    class PluginSlot& pluginSlot;  // owned by the AudioEngine's ChannelStrip
    AudioEngine& engine;            // for transport playhead + isPlaying queries
                                    // during Write capture (3c-ii)
    std::array<juce::uint32, ChannelStripParams::kNumBuses> lastBusColours {};  // for change detection in timerCallback
    float displayedGrDb = 0.0f;     // smoothed GR for the comp meter
    float displayedInputDb = -100.0f; // smoothed input level (L) for the level meter
    float inputPeakHoldDb = -100.0f;  // brief peak hold marker (L)
    int   inputPeakHoldFrames = 0;
    float displayedInputRDb = -100.0f;   // R-channel smoothed level (stereo mode)
    float inputPeakHoldRDb  = -100.0f;
    int   inputPeakHoldRFrames = 0;

    juce::Label nameLabel;

    // Per-channel insert plugin slot. Sits between the input row and the
    // EQ section. Click-left to load (or replace) a plugin via file chooser;
    // click-right to unload. Plugin name is shown on the button when loaded.
    // Custom subclass that adds a right-click context menu so the user can
    // change/remove the loaded plugin without having to right-click the
    // strip body (which used to be the only path - undiscoverable).
    struct PluginSlotButton final : public juce::TextButton
    {
        using juce::TextButton::TextButton;
        std::function<void(const juce::MouseEvent&)> onRightClick;
        void mouseDown (const juce::MouseEvent& e) override
        {
            if (e.mods.isPopupMenu() && onRightClick)
            {
                onRightClick (e);
                return;   // do NOT pass to base - we don't want a stuck "down" state
            }
            juce::TextButton::mouseDown (e);
        }
    };
    PluginSlotButton pluginSlotButton { "+ Plugin" };
    juce::String     lastSlotName;          // for change detection in timerCallback

    juce::Rectangle<int> eqArea, compArea;

    juce::Slider hpfKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label  hpfLabel;

    juce::Label  eqHeaderLabel;
    juce::TextButton eqTypeButton { "B" };
    struct BandRow
    {
        std::unique_ptr<juce::Slider> gain;
        std::unique_ptr<juce::Slider> freq;
        std::unique_ptr<juce::Slider> q;     // populated for bell-only mid bands (HM/LM); nullptr for shelf bands (HF/LF)
        juce::Label labelLeft, labelRight;
        juce::Label rowLabel;
        juce::Label qLabel;   // "Q" header above the Q knob (bell bands only)
    };
    std::array<BandRow, 4> eqRows;

    // COMP section. UniversalCompressor exposes a different control set per
    // mode (Opto/FET/VCA model real hardware), so the strip holds the union
    // of all three and shows only the ones for the current mode.
    juce::TextButton compOnButton { "COMP" };  // header doubles as on/off toggle
    juce::TextButton compModeOpto { "OPT" };
    juce::TextButton compModeFet  { "FET" };
    juce::TextButton compModeVca  { "VCA" };

    // Opto (LA-2A): peak-reduction knob + gain knob + LIMIT toggle.
    juce::Slider     optoPeakRedKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     optoGainKnob    { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label      optoPeakRedLabel, optoGainLabel;
    juce::TextButton optoLimitButton { "LIMIT" };

    // FET (1176): input + output + attack + release + ratio knob (5-step).
    juce::Slider     fetInputKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetOutputKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetAttackKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetReleaseKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     fetRatioKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label      fetInputLabel, fetOutputLabel, fetAttackLabel, fetReleaseLabel, fetRatioLabel;

    // VCA (classic): threshold via meter-strip drag handle (NOT a knob);
    // ratio + attack + release + output as knobs.
    juce::Slider     vcaRatioKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     vcaAttackKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     vcaReleaseKnob { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Slider     vcaOutputKnob  { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox };
    juce::Label      vcaRatioLabel, vcaAttackLabel, vcaReleaseLabel, vcaOutputLabel;

    std::unique_ptr<CompMeterStrip> compMeter;

    std::array<std::unique_ptr<juce::TextButton>, ChannelStripParams::kNumBuses> busButtons;

    juce::Slider panKnob   { juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::TextBoxBelow };
    juce::Label  panLabel;
    juce::Slider faderSlider { juce::Slider::LinearVertical, juce::Slider::TextBoxBelow };
    juce::Rectangle<int> inputMeterArea;
    juce::Rectangle<int> meterScaleArea;
    juce::Label inputPeakLabel;
    juce::Label grPeakLabel;       // GR readout (dB), sits to the right of inputPeakLabel
    juce::Label threshMeterLabel;  // "THR" header above CompMeterStrip's drag handle
    juce::TextButton muteButton    { "M" };
    juce::TextButton soloButton    { "S" };
    juce::TextButton phaseButton   { juce::CharPointer_UTF8 ("\xc3\x98") };  // Ø - phase invert

    // Automation mode cycle: OFF (dark) <-> READ (green). Phase 3c-i wires
    // only Off/Read; the underlying enum already covers Write/Touch which
    // 3c-ii will surface here. The label updates to mirror the current
    // mode (e.g. "OFF", "READ"). Right-click reserved for future per-param
    // mode picker (also 3c-ii).
    juce::TextButton autoModeButton { "OFF" };
    void onAutoModeClicked();
    void refreshAutoModeButton();
    // Append (normalized) `denormValue` to track.automationLanes[param]
    // at the current transport playhead. Maintains strict ascending
    // ordering so evaluateLane's binary search stays correct. Same-
    // sample writes coalesce; loop wraparound truncates future points.
    void captureWritePoint (AutomationParam param, float denormValue);
    // Last live fader dB rendered into the slider, polled in timerCallback
    // from track.strip.liveFaderDb. We avoid driving setValue every tick
    // by gating on a small delta - prevents UI churn when manual mode just
    // mirrors the user's setpoint and nothing is moving.
    float displayedLiveFaderDb = 0.0f;
    // Same idea for the pan knob - polled from track.strip.livePan.
    float displayedLivePan = 0.0f;
    juce::TextButton armButton     { "ARM" };
    juce::TextButton monitorButton { "IN"  };
    juce::TextButton printButton   { "PRINT" };  // print EQ/comp to recording
    juce::ComboBox   modeSelector;          // Mono / Stereo / MIDI
    juce::ComboBox   inputSelector;         // mono / stereo-L / (hidden in MIDI)
    juce::ComboBox   inputSelectorR;        // stereo-R (visible only in stereo mode)
    juce::ComboBox   midiInputSelector;     // MIDI input port (visible only in MIDI mode)

    // Aux send knobs (visible only in Mixing stage). Each sends a copy of
    // the channel signal into the matching AUX strip's plugin chain. The
    // PRE/POST tap point is per-channel-per-aux (auxSendPreFader[i]) and
    // gets its UI in Phase D.
    juce::Label  auxRowLabel;
    std::array<std::unique_ptr<juce::Slider>, ChannelStripParams::kNumAuxSends> auxKnobs;
    std::array<juce::Label,                  ChannelStripParams::kNumAuxSends> auxKnobLabels;
    bool mixingMode = false;
    juce::Rectangle<int> auxRowArea;

    void onHpfKnobChanged();
    void onInputSelectorChanged();
    void onTrackModeChanged();
    void refreshInputSelectorVisibility();
    void showColourMenu();
    void applyTrackColour (juce::Colour c);
    void refreshEqTypeButton();
    void setCompMode (int modeIndex);
    void refreshCompModeButtons();

    // Plugin slot UX. openPluginPicker is the main entry point - shows a
    // PopupMenu of installed plugins (from the scanned KnownPluginList) +
    // "Scan plugins" + "Browse for file...". The file-chooser path is the
    // fallback for unscanned formats / .vst3 bundles outside default paths.
    void openPluginPicker();
    void runPluginScanModal();
    void openPluginFileChooser();
    void unloadPluginSlot();
    void refreshPluginSlotButton();
    // Right-click context menu for the plugin slot button. Mirrors the
    // plugin items inside showColourMenu() but is reachable directly from
    // the slot button so users don't have to discover the strip-body
    // right-click. Shown by PluginSlotButton::onRightClick.
    void showPluginSlotMenu();
    void togglePluginEditor();              // open if closed, close if open
    void openPluginEditor();
    void closePluginEditor();
    std::unique_ptr<juce::FileChooser> activePluginChooser;
    juce::Component::SafePointer<juce::DialogWindow> activePluginEditorDialog;

    // Compact-mode plumbing.
    bool compactMode = false;
    juce::TextButton eqCompactButton   { "EQ" };
    juce::TextButton compCompactButton { "COMP" };
    // EQ + Comp use CallOutBox (in-window overlay, click-outside-to-dismiss
    // with the click consumed so the trigger button doesn't bounce-reopen).
    // Plugin editor stays as a top-level DialogWindow because plugin GUIs
    // can be very large and benefit from being a separate OS window.
    juce::Component::SafePointer<juce::CallOutBox> activeEqBox;
    juce::Component::SafePointer<juce::CallOutBox> activeCompBox;
    void openEqEditorPopup();
    void openCompEditorPopup();

    // Apply visibility to every EQ child / every COMP child in one shot -
    // the strip has a lot of controls split across the two sections so we
    // gather them here rather than scattering setVisible calls.
    void setEqSectionVisible (bool visible);
    void setCompSectionVisible (bool visible);
};
} // namespace focal
