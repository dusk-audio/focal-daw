#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include "DimOverlay.h"

namespace focal
{
// Small helper that wraps the recurring "DimOverlay + centred panel"
// pattern used by the piano roll, tuner, and (post-conversion) audio
// settings / mixdown / bounce / channel plugin editor.
//
// One ModalHost member on the host component. show() takes ownership of
// the panel body, displays the dim overlay + body sized to the host, and
// wires Esc / click-outside dismiss. close() tears it all down. The panel
// body can call host.closeModal() through an onDismiss callback when it
// has its own Cancel/Close button.
//
// The body is sized to its current `getWidth()` / `getHeight()` and
// centred. Pre-sizing on the body before calling show() is the caller's
// job - matches how juce::DialogWindow already worked.
//
// EmbeddedModal paints its own opaque rounded-panel backdrop behind the
// body so panels that don't fill their own background (e.g. raw juce::
// Component subclasses without paint() / setOpaque) still render solid
// instead of letting the channel strips bleed through.
class EmbeddedModal final : private juce::KeyListener
{
public:
    EmbeddedModal()  = default;
    ~EmbeddedModal() override { close(); }

    // Show `body` centred over `parent`. Click on the dim or Esc keypress
    // fires onDismiss (which usually just calls close()). Caller may
    // chain teardown logic onto onDismiss for things like clearing
    // session state when the modal closes.
    void show (juce::Component& parent,
               std::unique_ptr<juce::Component> body,
               std::function<void()> onDismiss = {})
    {
        close();
        host = &parent;
        body_ = std::move (body);
        dim_ = std::make_unique<DimOverlay>();
        dim_->setBounds (parent.getLocalBounds());
        userOnDismiss = std::move (onDismiss);
        dim_->onClick = [this]
        {
            if (userOnDismiss) userOnDismiss();
            else close();
        };
        parent.addAndMakeVisible (dim_.get());

        const auto bounds = parent.getLocalBounds();
        const int w = juce::jmax (1, body_->getWidth());
        const int h = juce::jmax (1, body_->getHeight());
        const auto bodyBounds = bounds.withSizeKeepingCentre (
            juce::jmin (w, bounds.getWidth()  - 16),
            juce::jmin (h, bounds.getHeight() - 16));

        // Backdrop sized slightly larger than the body so its rounded
        // corners frame the panel. Added BEFORE the body so the body
        // paints on top of it.
        backdrop_ = std::make_unique<Backdrop>();
        backdrop_->setBounds (bodyBounds.expanded (kBackdropMargin));
        parent.addAndMakeVisible (backdrop_.get());

        body_->setBounds (bodyBounds);
        parent.addAndMakeVisible (body_.get());
        body_->setWantsKeyboardFocus (true);
        body_->grabKeyboardFocus();
        body_->addKeyListener (this);
    }

    // Tear-down. Idempotent. Safe to call when nothing is open.
    void close()
    {
        if (host == nullptr) return;
        if (body_     != nullptr) host->removeChildComponent (body_.get());
        if (backdrop_ != nullptr) host->removeChildComponent (backdrop_.get());
        if (dim_      != nullptr) host->removeChildComponent (dim_.get());
        body_.reset();
        backdrop_.reset();
        dim_.reset();
        host = nullptr;
        userOnDismiss = {};
    }

    bool isOpen() const noexcept { return body_ != nullptr; }

    // Body access for typed callers - useful when a host needs to push
    // updates into the panel (e.g. polling a meter). Returns nullptr
    // when nothing is open.
    juce::Component* getBody() const noexcept { return body_.get(); }

private:
    bool keyPressed (const juce::KeyPress& k, juce::Component*) override
    {
        if (k == juce::KeyPress::escapeKey)
        {
            if (userOnDismiss) userOnDismiss();
            else close();
            return true;
        }
        return false;
    }

    // Opaque rounded panel painted behind the body. Sized via
    // bodyBounds.expanded(kBackdropMargin) so its corners frame the body.
    class Backdrop final : public juce::Component
    {
    public:
        Backdrop() { setOpaque (false); setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override
        {
            const auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xff141418).withAlpha (0.55f));
            g.fillRoundedRectangle (r.translated (0.0f, 4.0f), 8.0f);
            g.setColour (juce::Colour (0xff202024));
            g.fillRoundedRectangle (r, 8.0f);
            g.setColour (juce::Colour (0xff3a3a42));
            g.drawRoundedRectangle (r.reduced (0.5f), 8.0f, 1.0f);
        }
    };

    static constexpr int kBackdropMargin = 6;

    juce::Component* host = nullptr;
    std::unique_ptr<DimOverlay> dim_;
    std::unique_ptr<Backdrop> backdrop_;
    std::unique_ptr<juce::Component> body_;
    std::function<void()> userOnDismiss;
};
} // namespace focal
