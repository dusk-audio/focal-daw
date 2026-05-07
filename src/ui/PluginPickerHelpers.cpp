#include "PluginPickerHelpers.h"
#include "../engine/PluginManager.h"
#include "../engine/PluginSlot.h"

namespace focal::pluginpicker
{
namespace
{
constexpr int kIdScan        = 9001;
constexpr int kIdBrowseFile  = 9002;

void showLoadFailureAlert (const juce::String& message)
{
    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::WarningIcon)
            .withTitle ("Plugin load failed")
            .withMessage (message.isEmpty() ? "Unknown error" : message)
            .withButton ("OK"),
        nullptr);
}
} // namespace

void runScanModal (PluginManager& manager)
{
    // PluginDirectoryScanner is internally synchronous when allowAsync=false;
    // we surface a tiny banner so the user knows we're working. A polished
    // UX would lift this onto a thread with a real progress bar - good
    // follow-up once the picker is in active use across surfaces.
    auto* dialog = new juce::AlertWindow ("Scanning plugins",
                                            "Looking through VST3 / LV2 install "
                                            "locations… this can take a few "
                                            "seconds the first time.",
                                            juce::MessageBoxIconType::NoIcon);
    dialog->setUsingNativeTitleBar (true);
    dialog->enterModalState (false /*not blocking*/);

    const int added = manager.scanInstalledPlugins();
    delete dialog;

    juce::AlertWindow::showAsync (
        juce::MessageBoxOptions()
            .withIconType (juce::MessageBoxIconType::InfoIcon)
            .withTitle ("Plugin scan complete")
            .withMessage (juce::String::formatted (
                "Added %d plugin%s to the picker. (Total known: %d)",
                added, added == 1 ? "" : "s",
                manager.getKnownPluginList().getNumTypes()))
            .withButton ("OK"),
        nullptr);
}

void openFileChooser (PluginSlot& slot,
                       std::unique_ptr<juce::FileChooser>& chooserOwner,
                       std::function<void()> onChange)
{
    // VST3 plugins are bundles (directories), so the chooser allows both
    // files and directories. canSelectDirectories lets the user pick the
    // .vst3 bundle root. Default location is platform-specific - macOS
    // installs to ~/Library/Audio/Plug-Ins/VST3; Linux uses ~/.vst3.
#if defined(__APPLE__)
    const auto defaultDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                .getChildFile ("Library/Audio/Plug-Ins/VST3");
#else
    const auto defaultDir = juce::File::getSpecialLocation (juce::File::userHomeDirectory)
                                .getChildFile (".vst3");
#endif
    chooserOwner = std::make_unique<juce::FileChooser> (
        "Select a plugin",
        defaultDir,
        "*.vst3;*.component;*.so;*.lv2");

    auto* chooserPtr = chooserOwner.get();
    chooserPtr->launchAsync (
        juce::FileBrowserComponent::openMode |
        juce::FileBrowserComponent::canSelectFiles |
        juce::FileBrowserComponent::canSelectDirectories,
        [&slot, &chooserOwner, onChange = std::move (onChange)] (const juce::FileChooser& chooser)
        {
            const auto file = chooser.getResult();

            if (file == juce::File())
            {
                // User cancelled - drop the chooser and bail. (Reset comes
                // last because the JUCE FileChooser keeps `chooser` alive
                // while this lambda runs, but the unique_ptr destruction
                // schedules its own teardown after we return.)
                chooserOwner.reset();
                return;
            }

            juce::String error;
            const bool ok = slot.loadFromFile (file, error);
            chooserOwner.reset();
            if (! ok) { showLoadFailureAlert (error); return; }
            if (onChange) onChange();
        });
}

void openPickerMenu (PluginSlot& slot,
                      juce::Component& target,
                      std::unique_ptr<juce::FileChooser>& chooserOwner,
                      std::function<void()> onChange,
                      juce::Point<int> screenPosition)
{
    auto& manager = slot.getManagerForUi();
    auto& known   = manager.getKnownPluginList();

    juce::PopupMenu menu;
    if (known.getNumTypes() == 0)
    {
        menu.addSectionHeader ("No plugins scanned yet");
    }
    else
    {
        // Manufacturer-grouped hierarchy. Menu IDs returned for plugins are
        // 1-based KnownPluginList indices; we reserve 9000+ for our own
        // action items (scan, browse).
        known.addToMenu (menu,
                          juce::KnownPluginList::sortByManufacturer,
                          /*dirsToIgnore*/ {});
    }
    menu.addSeparator();
    menu.addItem (kIdScan,       "Scan plugins (VST3 / LV2)...");
    menu.addItem (kIdBrowseFile, "Browse for file...");

    juce::Component::SafePointer<juce::Component> safeTarget (&target);
    auto* slotPtr = &slot;
    auto* chooserOwnerPtr = &chooserOwner;
    auto onChangeCopy = onChange;

    // Anchor on click position when supplied (large click-target buttons
    // would otherwise drop the menu at their top-left), otherwise on the
    // component's bounds.
    auto options = juce::PopupMenu::Options();
    if (screenPosition.x >= 0 && screenPosition.y >= 0)
        options = options.withTargetScreenArea (
            juce::Rectangle<int> (screenPosition.x, screenPosition.y, 1, 1));
    else
        options = options.withTargetComponent (&target);

    menu.showMenuAsync (options,
        [slotPtr, safeTarget, chooserOwnerPtr, onChangeCopy = std::move (onChangeCopy),
         screenPosition] (int result) mutable
        {
            if (safeTarget.getComponent() == nullptr) return;  // host UI gone
            if (result == 0) return;  // cancelled
            if (result == kIdScan)
            {
                runScanModal (slotPtr->getManagerForUi());
                // Reopen the picker so the user immediately sees the newly
                // scanned plugins without a second click. Preserve the
                // original anchor so the reopened menu lands in the same
                // visual spot.
                openPickerMenu (*slotPtr, *safeTarget.getComponent(),
                                  *chooserOwnerPtr, std::move (onChangeCopy),
                                  screenPosition);
                return;
            }
            if (result == kIdBrowseFile)
            {
                openFileChooser (*slotPtr, *chooserOwnerPtr, std::move (onChangeCopy));
                return;
            }

            // 1..N → KnownPluginList type. Use getIndexChosenByMenu - JUCE's
            // canonical decoder for menu IDs returned by addToMenu.
            auto& mgr   = slotPtr->getManagerForUi();
            auto& known = mgr.getKnownPluginList();
            const int idx = known.getIndexChosenByMenu (result);
            if (idx < 0 || idx >= known.getNumTypes()) return;

            const auto* desc = known.getType (idx);
            if (desc == nullptr) return;

            juce::String error;
            if (! slotPtr->loadFromDescription (*desc, error))
            {
                showLoadFailureAlert (error);
                return;
            }
            if (onChangeCopy) onChangeCopy();
        });
}
} // namespace focal::pluginpicker
