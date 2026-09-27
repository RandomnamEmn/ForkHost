//
//  IconMenu.cpp
//  Light Host
//
//  Created by Rolando Islas on 12/26/15.
//
//

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <BinaryData.h>
#include "IconMenu.hpp"
#include "PluginChain.hpp"
#include "PluginWindow.h"
#include "AudioSettingsComponent.hpp"
#include "AudioDeviceInitHelpers.hpp"
#include "HostTheme.hpp"
#include "NoneAudioDevice.hpp"
#include "DebugAudioDevice.hpp"
#include "IsolatedPluginScanner.hpp"
#include <algorithm>
#include <iostream>
#include <ctime>
#include <climits>
#include <memory>
#if JUCE_WINDOWS
#include "Windows.h"
#endif

namespace
{
constexpr int trayOpenMainItemId = 10;

void focusWindow (Component& window)
{
    Process::makeForegroundProcess();
    window.setVisible (true);
    window.setAlwaysOnTop (true);
    window.toFront (true);
    window.grabKeyboardFocus();
    window.setAlwaysOnTop (false);
}

Colour getPluginIndicatorColour (bool failed, bool bypassed)
{
    const auto& lookAndFeel = LookAndFeel::getDefaultLookAndFeel();
    if (bypassed) return lookAndFeel.findColour (LightHostTheme::bypassedPluginColourId);
    if (failed)   return lookAndFeel.findColour (LightHostTheme::failedPluginColourId);
    return lookAndFeel.findColour (LightHostTheme::activePluginColourId);
}
}

class IconMenu::PluginListWindow : public DocumentWindow, private ListBoxModel
{
public:
    PluginListWindow(IconMenu& owner_, AudioPluginFormatManager& pluginFormatManager_)
        : DocumentWindow("Select Plugins",
            LookAndFeel::getDefaultLookAndFeel().findColour(DocumentWindow::backgroundColourId),
            DocumentWindow::minimiseButton | DocumentWindow::closeButton),
        owner(owner_), pluginFormatManager(pluginFormatManager_)
    {
        setContentOwned(new ContentComponent(*this), true);
        setUsingNativeTitleBar (false);

        optionsButton.setButtonText("Options");
        optionsButton.onClick = [this] { showOptionsMenu(); };

        detailLabel.setText("Select a plugin", dontSendNotification);
        searchBox.setTextToShowWhenEmpty ("Search plug-ins...",
            LookAndFeel::getDefaultLookAndFeel().findColour (LightHostTheme::secondaryTextColourId));
        searchBox.onTextChange = [this] { filterPlugins(); };
        sortFilter.addItem ("Name A-Z", 1);
        sortFilter.addItem ("Format", 2);
        sortFilter.setSelectedId (getAppProperties().getUserSettings()->getIntValue ("pluginListSortOrder", 1),
                                  dontSendNotification);
        sortFilter.setTooltip ("Sort plug-ins by name or group them by file format");
        sortFilter.onChange = [this]
        {
            auto* settings = getAppProperties().getUserSettings();
            settings->setValue ("pluginListSortOrder", sortFilter.getSelectedId());
            settings->saveIfNeeded();
            filterPlugins();
        };
        formatFilter.onChange = [this] { filterPlugins(); };
        pluginListBox.setModel(this);
        pluginListBox.setColour (ListBox::backgroundColourId,
            LookAndFeel::getDefaultLookAndFeel().findColour (LightHostTheme::panelBackgroundColourId));
        pluginListBox.setColour(ListBox::outlineColourId, Colours::transparentBlack);
        pluginListBox.setRowHeight(24);
        rebuildList();
        setResizable(true, false);
        setResizeLimits(400, 300, 800, 1500);
        setTopLeftPosition(60, 60);

        restoreWindowStateFromString(getAppProperties().getUserSettings()->getValue("listWindowPos"));
        setVisible(true);
    }

    void applyHostTheme()
    {
        const auto& hostLookAndFeel = LookAndFeel::getDefaultLookAndFeel();
        setBackgroundColour (hostLookAndFeel.findColour (DocumentWindow::backgroundColourId));
        searchBox.setTextToShowWhenEmpty ("Search plug-ins...",
                                          hostLookAndFeel.findColour (LightHostTheme::secondaryTextColourId));
        pluginListBox.setColour (ListBox::backgroundColourId,
                                 hostLookAndFeel.findColour (LightHostTheme::panelBackgroundColourId));
        if (auto* content = getContentComponent())
        {
            content->setColour (DocumentWindow::backgroundColourId,
                                hostLookAndFeel.findColour (DocumentWindow::backgroundColourId));
            content->repaint();
        }
        LightHostTheme::refreshHostComponentTree (*this);
    }

    ~PluginListWindow()
    {
        getAppProperties().getUserSettings()->setValue("listWindowPos", getWindowStateAsString());
        clearContentComponent();
    }

    void closeButtonPressed() override
    {
        // Defer destruction to avoid use-after-free: the DocumentWindow base
        // class continues to access `this` after closeButtonPressed() returns,
        // so synchronously setting the unique_ptr to nullptr would destroy the
        // object while it is still in use.
        auto safeOwner = Component::SafePointer<IconMenu>(&owner);
        MessageManager::callAsync ([safeOwner]() {
            if (auto* ownerPtr = safeOwner.getComponent())
                ownerPtr->pluginListWindow = nullptr;
        });
    }

private:
    // --- Content component for layout ---
    class ContentComponent : public Component
    {
    public:
        ContentComponent(PluginListWindow& w) : window(w)
        {
            setOpaque(true);
            addAndMakeVisible(window.optionsButton);
            addAndMakeVisible(window.sortFilter);
            addAndMakeVisible(window.searchBox);
            addAndMakeVisible(window.formatFilter);
            addAndMakeVisible(window.detailLabel);
            addAndMakeVisible(window.pluginListBox);
        }

        void paint(Graphics& g) override
        {
            g.fillAll(findColour(DocumentWindow::backgroundColourId));
        }

        void resized() override
        {
            auto r = getLocalBounds();
            auto toolbar = r.removeFromTop(30).reduced(2, 2);

            window.optionsButton.setBounds(toolbar.removeFromLeft(76).reduced(2));
            window.sortFilter.setBounds(toolbar.removeFromLeft(112).reduced(2));
            window.searchBox.setBounds(toolbar.reduced(2));

            auto filterRow = r.removeFromTop(30).reduced(4, 2);
            window.formatFilter.setBounds(filterRow.removeFromLeft(150).reduced(2));
            window.detailLabel.setBounds(filterRow.reduced(4, 2));
            window.pluginListBox.setBounds(r.reduced(2));
        }

    private:
        PluginListWindow& window;
    };

    // --- Members ---
    IconMenu& owner;
    AudioPluginFormatManager& pluginFormatManager;
    TextButton optionsButton;
    ComboBox sortFilter;
    TextEditor searchBox;
    ComboBox formatFilter;
    Label detailLabel;
    ListBox pluginListBox;
    std::vector<PluginDescription> allPlugins;
    std::vector<PluginDescription> visiblePlugins;
    StringArray filterFormatNames;

    int getNumRows() override { return (int) visiblePlugins.size(); }

    void paintListBoxItem(int rowNumber, Graphics& g, int width, int height, bool rowIsSelected) override
    {
        if (rowIsSelected)
            g.fillAll (LookAndFeel::getDefaultLookAndFeel().findColour (LightHostTheme::selectionBackgroundColourId));

        if (rowNumber < 0 || rowNumber >= (int) visiblePlugins.size())
            return;

        const auto& plugin = visiblePlugins[(size_t) rowNumber];
        g.setColour(findColour(ListBox::textColourId));
        g.setFont(Font(13.0f));
        auto nameArea = juce::Rectangle<int>(4, 0, jmax(0, width - 90), height);
        auto formatArea = juce::Rectangle<int>(width - 84, 0, 80, height);
        g.drawText(plugin.name, nameArea, Justification::centredLeft, true);
        g.setColour(findColour(Label::textColourId).withAlpha(0.72f));
        g.drawText(getFormatDisplayName(plugin.pluginFormatName), formatArea,
                   Justification::centredRight, true);
    }

    void selectedRowsChanged(int lastRowSelected) override
    {
        if (lastRowSelected >= 0 && lastRowSelected < (int) visiblePlugins.size())
            showPluginDetails(visiblePlugins[(size_t) lastRowSelected]);
        else
            detailLabel.setText("Select a plug-in", dontSendNotification);
    }

    void listBoxItemDoubleClicked(int row, const MouseEvent&) override
    {
        if (row >= 0 && row < (int) visiblePlugins.size())
            addPluginToChain(visiblePlugins[(size_t) row]);
    }

    // --- Methods ---
    void showOptionsMenu()
    {
        PopupMenu menu;
        menu.addItem(1, "Scan for new or updated plug-ins...");
        menu.addItem(2, "Remove dead plug-ins from list");
        menu.addItem(3, "Clear plug-in list");
        menu.addItem(4, "Open plug-in scan log");
        menu.addItem(5, "Clear scan results...");

        Component::SafePointer<PluginListWindow> safeThis (this);
        menu.showMenuAsync(PopupMenu::Options(), [safeThis](int result) {
            if (auto* self = safeThis.getComponent())
            {
                if (result == 1) self->scanForPlugins();
                else if (result == 2) self->removeDeadPlugins();
                else if (result == 3) self->clearPluginList();
                else if (result == 4) self->openPluginScanLog();
                else if (result == 5) self->confirmClearScanResults();
            }
        });
    }

    void confirmClearScanResults()
    {
        Component::SafePointer<PluginListWindow> safeThis (this);
        AlertWindow::showOkCancelBox (
            AlertWindow::WarningIcon,
            "Clear plug-in scan results?",
            "This clears the scan failure history and captured helper stacks.",
            "Clear", "Cancel", this,
            ModalCallbackFunction::create ([safeThis] (int result)
            {
                if (result != 1)
                    return;

                if (auto* self = safeThis.getComponent())
                    if (self->owner.pluginScanLog != nullptr
                        && ! self->owner.pluginScanLog->clearResults())
                        NativeMessageBox::showMessageBoxAsync (
                            MessageBoxIconType::WarningIcon,
                            "Could not clear scan results",
                            "Light Host could not remove one or both scan log files.");
            }));
    }

    void openPluginScanLog()
    {
        if (owner.pluginScanLog == nullptr)
            return;

        auto report = owner.pluginScanLog->getReportFile();
        if (! report.existsAsFile())
            report.replaceWithText ("No plug-in scan failures have been recorded yet.\n");

        report.startAsProcess();
    }

    // --- Scan dialog ---
    class ScanDialogContent : public Component, private ListBoxModel
    {
    public:
        ScanDialogContent(PluginListWindow& pw, AudioPluginFormatManager& fmtMgr,
            KnownPluginList& pluginList, IconMenu& owner)
            : pluginWindow(pw), formatManager(fmtMgr),
              knownList(pluginList), iconMenu(owner)
        {
            String saved = getAppProperties().getUserSettings()->getValue("pluginScanPaths");
            if (saved.isNotEmpty())
                paths.addTokens(saved, ";", "");

            statusLabel.setText("Standard VST folders and any added folders will be scanned.",
                                dontSendNotification);
            listBox.setModel(this);
            addAndMakeVisible(statusLabel);
            addAndMakeVisible(listBox);
            addAndMakeVisible(addButton);
            addAndMakeVisible(removeButton);
            addAndMakeVisible(clearButton);
            addAndMakeVisible(scanButton);
            addAndMakeVisible(cancelButton);

            addButton.setButtonText("Add");
            removeButton.setButtonText("Remove");
            clearButton.setButtonText("Clear...");
            scanButton.setButtonText("Scan");
            cancelButton.setButtonText("Cancel");

            addButton.onClick = [this] { addPath(); };
            removeButton.onClick = [this] { removeSelectedPath(); };
            clearButton.onClick = [this] { clearList(); };
            scanButton.onClick = [this] { runScan(); };
            cancelButton.onClick = [this] { exitDialog(0); };

            setSize(500, 400);
        }

        void resized() override
        {
            auto r = getLocalBounds().reduced(8);
            auto bottomBar = r.removeFromBottom(28);
            auto midBar = r.removeFromBottom(28);

            auto scanW = bottomBar.getWidth() / 2;
            scanButton.setBounds(bottomBar.removeFromLeft(scanW).reduced(4));
            cancelButton.setBounds(bottomBar.reduced(4));

            auto btnW = midBar.getWidth() / 3;
            addButton.setBounds(midBar.removeFromLeft(btnW).reduced(4));
            removeButton.setBounds(midBar.removeFromLeft(btnW).reduced(4));
            clearButton.setBounds(midBar.reduced(4));

            auto statusBar = r.removeFromBottom(22);
            statusLabel.setBounds(statusBar.reduced(4));

            listBox.setBounds(r);
        }

    private:
        PluginListWindow& pluginWindow;
        AudioPluginFormatManager& formatManager;
        KnownPluginList& knownList;
        IconMenu& iconMenu;
        StringArray paths;
        ListBox listBox;
        TextButton addButton, removeButton, clearButton, scanButton, cancelButton;
        Label statusLabel;

        int getNumRows() override { return paths.size(); }

        void paintListBoxItem(int rowNumber, Graphics& g, int width, int height, bool rowIsSelected) override
        {
            if (rowIsSelected)
                g.fillAll (LookAndFeel::getDefaultLookAndFeel().findColour (LightHostTheme::selectionBackgroundColourId));
            g.setColour(findColour(ListBox::textColourId));
            g.setFont(Font(13.0f));
            g.drawText(paths[rowNumber], 4, 0, width - 8, height, Justification::centredLeft, true);
        }

        void addPath()
        {
            FileChooser chooser("Select a plug-in folder to scan");
            if (chooser.browseForDirectory())
            {
                paths.add(chooser.getResult().getFullPathName());
                listBox.updateContent();
                savePaths();
            }
        }

        void removeSelectedPath()
        {
            int sel = listBox.getSelectedRow();
            if (sel >= 0 && sel < paths.size())
            {
                paths.remove(sel);
                listBox.updateContent();
                listBox.deselectAllRows();
                savePaths();
            }
        }

        void clearList()
        {
            pluginWindow.clearPluginList();
        }

        void runScan()
        {
            savePaths();
            scanButton.setEnabled(false);
            scanButton.setButtonText("Scanning...");
            statusLabel.setText("Scanning...", dontSendNotification);
            iconMenu.pluginScanLog->beginScan (knownList.getBlacklistedFiles());

            FileSearchPath searchPaths;
            // Each format supplies its platform-standard locations. Custom
            // folders remain additive instead of replacing those defaults.
            for (int i = 0; i < formatManager.getNumFormats(); ++i)
                if (auto* format = formatManager.getFormat(i))
                    searchPaths.addPath(format->getDefaultLocationsToSearch());

            for (auto& p : paths)
                searchPaths.add(File(p));
            searchPaths.removeRedundantPaths();

            File deadMansPedal(getAppProperties().getUserSettings()
                ->getFile().getSiblingFile("RecentlyCrashedPluginsList"));

            for (int i = 0; i < formatManager.getNumFormats(); ++i)
            {
                auto* format = formatManager.getFormat(i);
                if (format == nullptr) continue;

                PluginDirectoryScanner scanner(knownList, *format,
                    searchPaths, true, deadMansPedal);

                String pluginName;
                while (scanner.scanNextFile(true, pluginName))
                {
                    statusLabel.setText("Scanning: " + scanner.getNextPluginFileThatWillBeScanned(), dontSendNotification);
                    MessageManager::getInstance()->runDispatchLoopUntil(2);
                }
            }

            iconMenu.pluginScanLog->finishScan();
            const auto failedCount = iconMenu.pluginScanLog->getEntryCount();
            statusLabel.setText(failedCount == 0
                ? "Scan complete. Standard and added folders were checked."
                : String(failedCount) + " plug-ins failed or were skipped; open Options > Open plug-in scan log.",
                dontSendNotification);
            scanButton.setButtonText("Scan");
            scanButton.setEnabled(true);
            pluginWindow.rebuildList();
        }

        void savePaths()
        {
            getAppProperties().getUserSettings()->setValue("pluginScanPaths",
                paths.joinIntoString(";"));
            getAppProperties().saveIfNeeded();
        }

        void exitDialog(int result)
        {
            if (auto* dw = findParentComponentOfClass<DialogWindow>())
                dw->exitModalState(result);
        }
    };

    void scanForPlugins()
    {
        DialogWindow::LaunchOptions opts;
        opts.content.setOwned(new ScanDialogContent(*this, pluginFormatManager,
            owner.knownPluginList, owner));
        opts.dialogTitle = "Scan for Plugins";
        opts.componentToCentreAround = this;
        opts.escapeKeyTriggersCloseButton = true;
        opts.useNativeTitleBar = false;
        opts.dialogBackgroundColour = LookAndFeel::getDefaultLookAndFeel().findColour(DocumentWindow::backgroundColourId);
        opts.resizable = false;
        opts.runModal();
        rebuildList();
    }

    void removeDeadPlugins()
    {
        for (int i = owner.knownPluginList.getNumTypes() - 1; i >= 0; i--)
        {
            if (auto* desc = owner.knownPluginList.getType(i))
            {
                if (!pluginFormatManager.doesPluginStillExist(*desc))
                    owner.knownPluginList.removeType(*desc);
            }
        }
        rebuildList();
    }

    void clearPluginList()
    {
        owner.knownPluginList.clear();
        owner.knownPluginList.clearBlacklistedFiles();
        auto settingsFile = getAppProperties().getUserSettings()->getFile();
        settingsFile.getSiblingFile("RecentlyCrashedPluginsList").deleteFile();
        rebuildList();
    }

    void rebuildList()
    {
        const auto selectedFormat = getFormatNameForItem(formatFilter.getSelectedItemIndex());
        allPlugins.clear();
        for (int i = 0; i < owner.knownPluginList.getNumTypes(); ++i)
        {
            if (auto* desc = owner.knownPluginList.getType(i))
                allPlugins.push_back(*desc);
        }

        StringArray formats;
        for (int i = 0; i < pluginFormatManager.getNumFormats(); ++i)
            if (auto* format = pluginFormatManager.getFormat(i))
                formats.addIfNotAlreadyThere(format->getName());
        for (const auto& plugin : allPlugins)
            formats.addIfNotAlreadyThere(plugin.pluginFormatName);
        formats.sort(true);

        formatFilter.onChange = nullptr;
        formatFilter.clear(dontSendNotification);
        formatFilter.addItem("All formats", 1);
        filterFormatNames = formats;
        for (int i = 0; i < formats.size(); ++i)
            formatFilter.addItem(getFormatDisplayName(formats[i]), i + 2);
        int selectedId = 1;
        // Restore the format by its visible text when possible.
        for (int i = 0; i < formatFilter.getNumItems(); ++i)
            if (getFormatNameForItem(i) == selectedFormat)
                selectedId = formatFilter.getItemId(i);
        formatFilter.setSelectedId(selectedId, dontSendNotification);
        formatFilter.onChange = [this] { filterPlugins(); };
        filterPlugins();
    }

    void filterPlugins()
    {
        const auto selectedFormat = getFormatNameForItem(formatFilter.getSelectedItemIndex());
        const auto query = searchBox.getText().trim();
        visiblePlugins.clear();
        for (const auto& plugin : allPlugins)
        {
            if (selectedFormat.isNotEmpty() && plugin.pluginFormatName != selectedFormat)
                continue;

            if (query.isNotEmpty()
                && !plugin.name.containsIgnoreCase(query)
                && !plugin.manufacturerName.containsIgnoreCase(query))
                continue;

            visiblePlugins.push_back(plugin);
        }

        // Sort the final filtered rows, so changing sort order always affects
        // the exact collection that ListBox paints.
        const bool sortByFormat = sortFilter.getSelectedId() == 2;
        std::stable_sort (visiblePlugins.begin(), visiblePlugins.end(),
            [sortByFormat] (const PluginDescription& a, const PluginDescription& b)
            {
                if (sortByFormat)
                {
                    const auto formatComparison = a.pluginFormatName.compareIgnoreCase (b.pluginFormatName);
                    if (formatComparison != 0)
                        return formatComparison < 0;
                }

                const auto nameComparison = a.name.compareIgnoreCase (b.name);
                if (nameComparison != 0)
                    return nameComparison < 0;

                return sortByFormat
                    ? a.pluginFormatName.compareIgnoreCase (b.pluginFormatName) < 0
                    : a.manufacturerName.compareIgnoreCase (b.manufacturerName) < 0;
            });

        pluginListBox.updateContent();
        pluginListBox.repaint();
        pluginListBox.deselectAllRows();
        detailLabel.setText("" + String(visiblePlugins.size()) + " plug-ins", dontSendNotification);
    }

    static String getFormatDisplayName(const String& formatName)
    {
        return formatName == "VST" ? "VST2" : formatName;
    }

    String getFormatNameForItem(int itemIndex) const
    {
        if (itemIndex <= 0 || itemIndex > filterFormatNames.size())
            return {};

        return filterFormatNames[itemIndex - 1];
    }

    void showPluginDetails(const PluginDescription& desc)
    {
        String info = desc.category + " / " + desc.pluginFormatName + " / v" + desc.version;
        detailLabel.setText(info, dontSendNotification);
    }

    void addPluginToChain(const PluginDescription& desc)
    {
        owner.pluginChain->add(desc);
        owner.markPresetDirty();
        PluginWindow::updateAllTitlesAndToolbars (&owner);

        auto safeOwner = Component::SafePointer<IconMenu>(&owner);
        MessageManager::callAsync([safeOwner]() {
            if (auto* ownerPtr = safeOwner.getComponent())
            {
                ownerPtr->pluginListWindow = nullptr;
                ownerPtr->refreshMainWindow();
            }
        });
    }
};

class IconMenu::PluginRackComponent : public Component,
                                      public DragAndDropTarget,
                                      private ListBoxModel
{
    class RackRow : public Component
    {
        class BypassDot : public Component, public SettableTooltipClient
        {
        public:
            explicit BypassDot (RackRow& row_) : row (row_) {}
            void setStatus (bool shouldBeBypassed, bool hasFailed)
            {
                bypassed = shouldBeBypassed;
                failed = hasFailed;
                repaint();
            }
            void paint (Graphics& g) override
            {
                auto dot = getLocalBounds().toFloat().withSizeKeepingCentre (18.0f, 18.0f);
                g.setColour (getPluginIndicatorColour (failed, bypassed));
                g.fillEllipse (dot);
                if (isMouseOver())
                {
                    g.setColour (LookAndFeel::getDefaultLookAndFeel()
                                     .findColour (Label::textColourId).withAlpha (0.45f));
                    g.drawEllipse (dot, 1.5f);
                }
            }
            void mouseDown (const MouseEvent& e) override { leftButtonWasPressed = e.mods.isLeftButtonDown(); }
            void mouseUp (const MouseEvent& e) override
            {
                if (leftButtonWasPressed && e.mouseWasClicked())
                    row.owner.icon.togglePluginBypass (row.rowIndex);
                leftButtonWasPressed = false;
            }
            void mouseEnter (const MouseEvent&) override { repaint(); }
            void mouseExit (const MouseEvent&) override { repaint(); }
        private:
            RackRow& row;
            bool bypassed = false;
            bool failed = false;
            bool leftButtonWasPressed = false;
        };

        class RackRowMouseListener : public MouseListener
        {
        public:
            explicit RackRowMouseListener (RackRow& row_) : row (row_) {}
            void mouseDown (const MouseEvent& event) override
            {
                middleButtonWasPressed = event.mods.isMiddleButtonDown();
            }
            void mouseUp (const MouseEvent& event) override
            {
                if (middleButtonWasPressed && event.mouseWasClicked())
                    row.owner.icon.reloadPluginAt (row.rowIndex);
                middleButtonWasPressed = false;
            }
        private:
            RackRow& row;
            bool middleButtonWasPressed = false;
        };

        class DragBar : public Component
        {
        public:
            explicit DragBar (RackRow& row_) : row (row_) {}
            void paint (Graphics& g) override
            {
                auto bounds = getLocalBounds().toFloat();
                g.setColour (LookAndFeel::getDefaultLookAndFeel().findColour (DocumentWindow::backgroundColourId).contrasting (0.10f));
                g.fillRect (bounds);
                g.setColour (LookAndFeel::getDefaultLookAndFeel().findColour (Label::textColourId).withAlpha (0.7f));
                for (int i = -1; i <= 1; ++i)
                    g.drawLine (bounds.getCentreX() + (float) i * 4.0f, bounds.getCentreY() - 5.0f,
                                bounds.getCentreX() + (float) i * 4.0f, bounds.getCentreY() + 5.0f, 1.5f);
            }
            void mouseDown (const MouseEvent& event) override
            {
                dragStarted = false;
                leftButtonWasPressed = event.mods.isLeftButtonDown();
                rightButtonWasPressed = event.mods.isRightButtonDown();
            }
            void mouseDrag (const MouseEvent& event) override
            {
                if (! leftButtonWasPressed || dragStarted || event.getDistanceFromDragStart() < 5) return;
                if (auto* container = DragAndDropContainer::findParentDragContainerFor (&row.owner))
                {
                    dragStarted = true;
                    Image dragImage (Image::ARGB, 300, 38, true);
                    Graphics g (dragImage);
                    g.setColour (LookAndFeel::getDefaultLookAndFeel().findColour (DocumentWindow::backgroundColourId));
                    g.fillRect (0.0f, 0.0f, 300.0f, 38.0f);
                    g.setColour (getPluginIndicatorColour (row.failed, row.bypassed));
                    g.fillEllipse (10.0f, 11.0f, 16.0f, 16.0f);
                    g.setColour (LookAndFeel::getDefaultLookAndFeel().findColour (Label::textColourId));
                    g.setFont (FontOptions (14.0f));
                    g.drawText (row.pluginName, 36, 0, 254, 38, Justification::centredLeft, true);
                    const Point<int> imageOffsetFromMouse (12, -19);
                    container->startDragging ("plugin-row:" + String (row.rowIndex), this,
                                              ScaledImage (dragImage), false, &imageOffsetFromMouse,
                                              &event.source);
                }
            }
            void mouseUp (const MouseEvent& event) override
            {
                if (rightButtonWasPressed && event.mouseWasClicked())
                    row.owner.icon.renamePluginAt (row.rowIndex);

                leftButtonWasPressed = false;
                rightButtonWasPressed = false;
            }
        private:
            RackRow& row;
            bool dragStarted = false;
            bool leftButtonWasPressed = false;
            bool rightButtonWasPressed = false;
        };

    public:
        explicit RackRow (PluginRackComponent& owner_)
            : owner (owner_), bypassButton (*this), dragBar (*this), mouseListener (*this)
        {
            addAndMakeVisible (dragBar);
            addAndMakeVisible (bypassButton);
            addAndMakeVisible (nameLabel);
            addMouseListener (&mouseListener, true);
            nameLabel.setInterceptsMouseClicks (false, false);
        }
        void update (int index)
        {
            rowIndex = index;
            if (index < 0 || index >= owner.icon.pluginChain->size()) return;
            const auto& slot = (*owner.icon.pluginChain)[index];
            bypassed = slot.bypassed;
            failed = slot.isFailed();
            pluginName = owner.icon.pluginChain->getDisplayName (index);
            bypassButton.setStatus (bypassed, failed);
            bypassButton.setTooltip (bypassed ? "Click to enable this plug-in" : "Click to bypass this plug-in");
            nameLabel.setText (String (index + 1) + ".  " + pluginName
                               + (slot.isFailed() ? "  (Failed)" : ""), dontSendNotification);
        }
        void mouseDoubleClick (const MouseEvent& event) override
        {
            if (event.mods.isRightButtonDown()) owner.icon.removePluginAt (rowIndex);
            else if (event.mods.isLeftButtonDown()) owner.icon.openPluginEditor (rowIndex);
        }
        void paint (Graphics& g) override
        {
            g.setColour (LookAndFeel::getDefaultLookAndFeel().findColour (DocumentWindow::backgroundColourId).contrasting (0.10f));
            g.drawRect (getLocalBounds().toFloat().reduced (0.5f), 1.0f);
        }
        void resized() override
        {
            auto area = getLocalBounds().reduced (6, 4);
            dragBar.setBounds (area.removeFromLeft (42).reduced (4, 3));
            bypassButton.setBounds (area.removeFromLeft (32).reduced (2));
            nameLabel.setBounds (area.reduced (6, 0));
        }
    private:
        PluginRackComponent& owner;
        int rowIndex = -1;
        String pluginName;
        bool bypassed = false;
        bool failed = false;
        BypassDot bypassButton;
        DragBar dragBar;
        Label nameLabel;
        RackRowMouseListener mouseListener;
    };

    class InfoButton final : public Component,
                             private Timer
    {
    public:
        InfoButton()
        {
            setWantsKeyboardFocus (false);
        }

        std::function<void (bool)> onHover;

        void paint (Graphics& g) override
        {
            auto bounds = getLocalBounds().toFloat().reduced (3.0f);
            const auto& laf = LookAndFeel::getDefaultLookAndFeel();
            auto colour = laf.findColour (Label::textColourId);
            if (isMouseOver()) colour = colour.brighter (0.2f);

            g.setColour (colour);
            g.drawEllipse (bounds, 1.5f);
            g.setFont (FontOptions (12.0f, Font::bold));
            g.drawText ("i", bounds.toNearestInt(), Justification::centred);
        }

        void mouseEnter (const MouseEvent&) override { startTimer (700); }
        void mouseExit (const MouseEvent&) override
        {
            stopTimer();
            if (onHover != nullptr)
                onHover (false);
        }

    private:
        void timerCallback() override
        {
            stopTimer();
            if (onHover != nullptr)
                onHover (true);
        }
    };

    class InfoBubble final : public Component
    {
    public:
        InfoBubble()
        {
            setInterceptsMouseClicks (false, false);
        }

        void paint (Graphics& g) override
        {
            auto bounds = getLocalBounds();
            const auto& laf = LookAndFeel::getDefaultLookAndFeel();
            g.setColour (laf.findColour (TooltipWindow::backgroundColourId));
            g.fillRect (bounds);
            g.setColour (laf.findColour (TooltipWindow::outlineColourId));
            g.drawRect (bounds.reduced (1), 1);
            g.setColour (laf.findColour (TooltipWindow::textColourId));
            g.setFont (FontOptions (12.0f));
            g.drawFittedText ("Click a plug-in's green dot to bypass it. "
                              "Middle-click a plug-in row to reload it. "
                              "Double left-click a plug-in row to edit it. "
                              "Double right-click a plug-in row to remove it. "
                              "Left-drag a plug-in's grip to reorder it. "
                              "Right-click the grip to set the instance name.",
                              bounds.reduced (8), Justification::topLeft, 6);
        }
    };

public:
    explicit PluginRackComponent (IconMenu& owner_)
        : icon (owner_), listBox ("Active Plugins", this)
    {
        setOpaque (true);
        listBox.setRowHeight (42);
        listBox.setColour (ListBox::backgroundColourId,
            LookAndFeel::getDefaultLookAndFeel().findColour (LightHostTheme::panelBackgroundColourId));
        addButton.setButtonText ("Add plugins...");
        addButton.onClick = [this] { icon.reloadPlugins(); };
        infoButton.onHover = [this] (bool shouldShow)
        {
            infoBubble.setVisible (shouldShow);
            if (shouldShow)
                infoBubble.toFront (false);
        };
        infoBubble.setVisible (false);
        addAndMakeVisible (addButton);
        addAndMakeVisible (infoButton);
        addAndMakeVisible (listBox);
        addChildComponent (infoBubble);
    }
    ~PluginRackComponent() override { listBox.setModel (nullptr); }

    void paint (Graphics& g) override
    {
        g.fillAll (LookAndFeel::getDefaultLookAndFeel()
                     .findColour (LightHostTheme::panelBackgroundColourId));
    }
    bool isInterestedInDragSource (const SourceDetails& details) override
    {
        return details.description.toString().startsWith ("plugin-row:");
    }
    void itemDropped (const SourceDetails& details) override
    {
        if (! isInterestedInDragSource (details)) return;
        const int from = details.description.toString().fromFirstOccurrenceOf (":", false, false).getIntValue();
        const auto point = listBox.getLocalPoint (this, details.localPosition);
        const int rowAtDrop = listBox.getRowContainingPosition (point.x, point.y);
        const int rowHeight = jmax (1, listBox.getRowHeight());
        const int insertion = rowAtDrop < 0 ? (point.y < 0 ? 0 : icon.pluginChain->size())
            : rowAtDrop + ((point.y % rowHeight) >= rowHeight / 2 ? 1 : 0);
        const int to = insertion > from ? insertion - 1 : insertion;
        if (to != from) icon.movePluginTo (from, to);
    }
    void refresh()
    {
        listBox.setColour (ListBox::backgroundColourId,
            LookAndFeel::getDefaultLookAndFeel().findColour (LightHostTheme::panelBackgroundColourId));
        listBox.updateContent();
        listBox.repaint();
    }
    void resized() override
    {
        auto area = getLocalBounds().reduced (8);
        auto toolbar = area.removeFromTop (34);
        addButton.setBounds (toolbar.removeFromRight (142));
        infoButton.setBounds (toolbar.removeFromRight (30).withSizeKeepingCentre (24, 24));
        infoBubble.setBounds (jmax (8, getWidth() - 328), 40,
                              jmin (320, getWidth() - 16), 116);
        area.removeFromTop (4);
        listBox.setBounds (area);
    }
private:
    int getNumRows() override { return icon.pluginChain->size(); }
    void paintListBoxItem (int, Graphics&, int, int, bool) override {}
    Component* refreshComponentForRow (int row, bool, Component* existing) override
    {
        auto* component = dynamic_cast<RackRow*> (existing);
        if (component == nullptr) component = new RackRow (*this);
        component->update (row);
        return component;
    }
    IconMenu& icon;
    ListBox listBox;
    TextButton addButton;
    InfoButton infoButton;
    InfoBubble infoBubble;
};

class IconMenu::PresetBrowserComponent : public Component, private ListBoxModel
{
public:
    explicit PresetBrowserComponent (IconMenu& owner_) : owner (owner_), listBox ("Presets", this)
    {
        listBox.setRowHeight (28);
        loadButton.setButtonText ("Load selected");
        browseButton.setButtonText ("Load file...");
        newButton.setButtonText ("New");
        saveButton.setButtonText ("Save");
        saveAsButton.setButtonText ("Save As...");
        loadButton.onClick = [this] { loadSelected(); };
        browseButton.onClick = [this] { browseForPreset(); };
        newButton.onClick = [this] {
            owner.createNewPreset();
            refresh();
        };
        saveButton.onClick = [this] { owner.saveCurrentPreset(); refresh(); };
        saveAsButton.onClick = [this] { owner.saveCurrentPresetAs(); refresh(); };
        addAndMakeVisible (listBox);
        addAndMakeVisible (loadButton);
        addAndMakeVisible (browseButton);
        addAndMakeVisible (newButton);
        addAndMakeVisible (saveButton);
        addAndMakeVisible (saveAsButton);
        refresh();
    }

    ~PresetBrowserComponent() override { listBox.setModel (nullptr); }

    void resized() override
    {
        auto area = getLocalBounds().reduced (8);
        auto buttons = area.removeFromBottom (34);
        newButton.setBounds (buttons.removeFromLeft (72).reduced (2));
        saveButton.setBounds (buttons.removeFromLeft (72).reduced (2));
        saveAsButton.setBounds (buttons.removeFromLeft (90).reduced (2));
        browseButton.setBounds (buttons.removeFromLeft (108).reduced (2));
        loadButton.setBounds (buttons.removeFromRight (126).reduced (2));
        listBox.setBounds (area);
    }

    void refresh()
    {
        presets = PresetManager::getDefaultPresetDirectory().findChildFiles (File::findFiles, false, "*.lhp");
        std::sort (presets.begin(), presets.end(), [] (const File& a, const File& b) {
            return a.getFileNameWithoutExtension().compareIgnoreCase (b.getFileNameWithoutExtension()) < 0;
        });
        listBox.updateContent();
        listBox.repaint();
    }

private:
    int getNumRows() override { return presets.size(); }

    void paintListBoxItem (int row, Graphics& g, int width, int height, bool selected) override
    {
        if (row < 0 || row >= presets.size()) return;
        if (selected) g.fillAll (listBox.findColour (LightHostTheme::selectionBackgroundColourId));
        g.setColour (listBox.findColour (ListBox::textColourId));
        g.drawText (presets.getReference (row).getFileNameWithoutExtension(),
                    8, 0, width - 16, height, Justification::centredLeft, true);
    }

    void listBoxItemDoubleClicked (int row, const MouseEvent&) override
    {
        if (row >= 0 && row < presets.size()) owner.loadPresetFile (presets.getReference (row));
    }

    void loadSelected()
    {
        const int row = listBox.getSelectedRow();
        if (row >= 0 && row < presets.size()) owner.loadPresetFile (presets.getReference (row));
    }

    void browseForPreset()
    {
        Process::makeForegroundProcess();
        FileChooser chooser ("Load Preset", PresetManager::getDefaultPresetDirectory(), "*.lhp");
        if (chooser.browseForFileToOpen()) owner.loadPresetFile (chooser.getResult());
    }

    IconMenu& owner;
    ListBox listBox;
    TextButton loadButton, browseButton, newButton, saveButton, saveAsButton;
    Array<File> presets;
};

class IconMenu::MainWindow : public DocumentWindow, public DragAndDropContainer
{
    class MainContent : public Component
    {
    public:
        explicit MainContent (IconMenu& host)
            : owner (host), rack (host), presets (host),
              audio (host.getDeviceManagerForUi(), host.getAudioStreamForUi(),
                     host.getPluginChain(), host.getLastDeviceError(),
                     host.isMidiServiceResponsive()),
              tabs (TabbedButtonBar::TabsAtTop)
        {
            const auto background = LookAndFeel::getDefaultLookAndFeel()
                .findColour (DocumentWindow::backgroundColourId);
            tabs.addTab ("Active Plugins", background, &rack, false);
            tabs.addTab ("Presets", background, &presets, false);
            tabs.addTab ("Audio & MIDI", background, &audio, false);
            themeLabel.setText ("Theme:", dontSendNotification);
            addAndMakeVisible (themeLabel);
            themeSelector.addItem ("Dark", 1);
            themeSelector.addItem ("Light", 2);
            themeSelector.addItem ("Midnight", 3);
            themeSelector.addItem ("Custom", 4);
            auto* settings = getAppProperties().getUserSettings();
            palette = LightHostTheme::load (*settings);
            const auto savedTheme = settings->getValue ("hostTheme", "Dark");
            themeSelector.setSelectedId (savedTheme == "Light" ? 2 : savedTheme == "Midnight" ? 3
                                         : savedTheme == "Custom" ? 4 : 1, dontSendNotification);
            themeSelector.onChange = [this]
            {
                auto* settings = getAppProperties().getUserSettings();
                if (themeSelector.getSelectedId() == 4)
                {
                    palette = LightHostTheme::loadCustom (*settings);
                    settings->setValue ("hostTheme", "Custom");
                    settings->saveIfNeeded();
                }
                else
                {
                    palette = LightHostTheme::preset (themeSelector.getText());
                    settings->setValue ("hostTheme", themeSelector.getText());
                    settings->saveIfNeeded();
                }
                applyPaletteToWindows();
            };
            addAndMakeVisible (themeSelector);
            customizeThemeButton.setButtonText ("Customize...");
            customizeThemeButton.onClick = [this] { openThemeEditor(); };
            addAndMakeVisible (customizeThemeButton);
            addAndMakeVisible (tabs);
        }

        ~MainContent() override { owner.persistAudioSettings (audio); }

        void resized() override
        {
            auto area = getLocalBounds();
            auto themeRow = area.removeFromTop (34).reduced (8, 3);
            customizeThemeButton.setBounds (themeRow.removeFromRight (104));
            themeRow.removeFromRight (8);
            themeSelector.setBounds (themeRow.removeFromRight (120));
            themeLabel.setBounds (themeRow.removeFromRight (48));
            tabs.setBounds (area);
        }
        void refresh() { rack.refresh(); presets.refresh(); }

        void openThemeEditor()
        {
            auto safeThis = Component::SafePointer<MainContent> (this);
            DialogWindow::LaunchOptions options;
            options.content.setOwned (new LightHostTheme::Editor (palette, [safeThis] (const LightHostTheme::Palette& updated)
            {
                if (auto* self = safeThis.getComponent())
                {
                    self->palette = updated;
                    self->themeSelector.setSelectedId (4, dontSendNotification);
                    LightHostTheme::saveCustom (*getAppProperties().getUserSettings(), self->palette);
                    self->applyPaletteToWindows();
                }
            }));
            options.dialogTitle = "Customize Light Host Colors";
            options.dialogBackgroundColour = LightHostTheme::customizationLookAndFeel()
                .findColour (ResizableWindow::backgroundColourId);
            options.escapeKeyTriggersCloseButton = true;
            options.useNativeTitleBar = true;
            options.resizable = true;
            options.launchAsync();
        }

        void applyPaletteToWindows()
        {
            if (auto* hostLookAndFeel = dynamic_cast<LookAndFeel_V4*> (&LookAndFeel::getDefaultLookAndFeel()))
                LightHostTheme::apply (*hostLookAndFeel, palette);
            if (auto* window = dynamic_cast<DocumentWindow*> (getTopLevelComponent()))
                window->setBackgroundColour (palette.window);
            tabs.setColour (TabbedComponent::backgroundColourId, palette.window);
            for (int i = 0; i < tabs.getNumTabs(); ++i)
                tabs.setTabBackgroundColour (i, palette.window);
            rack.refresh();
            presets.refresh();
            audio.repaint();
            owner.refreshThemeOnOpenWindows();
            if (auto* top = getTopLevelComponent())
                LightHostTheme::refreshHostComponentTree (*top);
        }
        void showAudioTab() { tabs.setCurrentTabIndex (2); }

    private:
        IconMenu& owner;
        Label themeLabel;
        ComboBox themeSelector;
        TextButton customizeThemeButton;
        LightHostTheme::Palette palette;
        PluginRackComponent rack;
        PresetBrowserComponent presets;
        AudioSettingsComponent audio;
        TabbedComponent tabs;
    };

public:
    explicit MainWindow (IconMenu& host)
        : DocumentWindow ("Light Host", LookAndFeel::getDefaultLookAndFeel()
                            .findColour (DocumentWindow::backgroundColourId),
                          DocumentWindow::minimiseButton | DocumentWindow::closeButton),
          owner (host)
    {
        setContentOwned (new MainContent (owner), true);
        setUsingNativeTitleBar (false);
        setResizable (true, true);
        setResizeLimits (520, 360, 1400, 1000);
        centreWithSize (860, 620);
        setVisible (true);
        focus();
    }

    void closeButtonPressed() override { setVisible (false); }

    void focus()
    {
        ::focusWindow (*this);
    }

    void refresh()
    {
        if (auto* content = dynamic_cast<MainContent*> (getContentComponent())) content->refresh();
    }

    void showAudioTab()
    {
        if (auto* content = dynamic_cast<MainContent*> (getContentComponent())) content->showAudioTab();
        focus();
    }

private:
    IconMenu& owner;
};

void IconMenu::refreshMainWindow()
{
    if (mainControlWindow != nullptr)
        mainControlWindow->refresh();
}

IconMenu::IconMenu (const HostOptions& options)
    : INDEX_EDIT(1000000), INDEX_BYPASS(2000000), INDEX_DELETE(3000000),
      INDEX_MOVE_UP(4000000), INDEX_MOVE_DOWN(5000000),
      INDEX_PRESET_SAVE(6000000), INDEX_PRESET_SAVE_AS(6000001),
      INDEX_PRESET_LOAD_SELECT(6000002), INDEX_PRESET_NEW(6000003),
      INDEX_PRESET_LOAD_FILE(7000000),
      INDEX_DEBUG_PROCESS_ONE(8000000), INDEX_DEBUG_PROCESS_HUNDRED(8000001),
      hostOptions (options)
{
    // Initialization
   #if JUCE_VERSION >= 0x080009
    addDefaultFormatsToManager (formatManager);
   #else
    formatManager.addDefaultFormats();
   #endif
#if JUCE_WINDOWS
    x = y = 0;
#endif
    // Register the Loopback audio device type (WASAPI loopback capture).
    // Must create default types first, then add our custom type, then call
    // initialise — this ensures both the standard types (Windows Audio, ASIO)
    // and "Loopback" are available, and the saved device state can correctly
    // restore "Loopback" if it was the previously selected type.
    {
        OwnedArray<AudioIODeviceType> defaultTypes;
        deviceManager.createAudioDeviceTypes(defaultTypes);
        for (auto* t : defaultTypes)
        {
            t->scanForDevices();
            deviceManager.addAudioDeviceType(std::unique_ptr<AudioIODeviceType>(t));
        }
        defaultTypes.clear(false);

        auto* loopbackType = new LoopbackAudioIODeviceType();
        loopbackType->scanForDevices();
        deviceManager.addAudioDeviceType(std::unique_ptr<AudioIODeviceType>(loopbackType));

        // Register the silent "None" device type — used as a fallback when
        // a preset's saved audio device type is not available on this system.
        auto* noneType = new NoneAudioIODeviceType();
        noneType->scanForDevices();
        deviceManager.addAudioDeviceType(std::unique_ptr<AudioIODeviceType>(noneType));

        if (hostOptions.debugMode)
        {
            auto* debugType = new DebugAudioIODeviceType();
            debugType->scanForDevices();
            deviceManager.addAudioDeviceType(std::unique_ptr<AudioIODeviceType>(debugType));
        }
    }

    // Audio device
    auto* settings = getAppProperties().getUserSettings();
    player.setDefaultBpm (settings->getDoubleValue ("defaultBpm", 0.0));
    player.fadeEnabled.store (settings->getBoolValue ("enableFade", player.fadeEnabled.load()));
    String audioInitError;

    if (hostOptions.debugMode)
    {
        // The Debug device has logical stereo I/O so JUCE prepares the graph
        // exactly like a normal host, but its start() method never invokes the
        // real-time audio callback. Debugger stops therefore cannot underrun,
        // deadlock, or trip an external host watchdog.
        deviceManager.setCurrentAudioDeviceType (DebugAudioIODevice::typeName(), false);

        AudioDeviceManager::AudioDeviceSetup debugSetup;
        debugSetup.inputDeviceName = DebugAudioIODevice::deviceName();
        debugSetup.outputDeviceName = DebugAudioIODevice::deviceName();
        debugSetup.sampleRate = hostOptions.sampleRate;
        debugSetup.bufferSize = hostOptions.blockSize;
        debugSetup.useDefaultInputChannels = true;
        debugSetup.useDefaultOutputChannels = true;

        audioInitError = deviceManager.initialise (2, 2, nullptr, false, {}, &debugSetup);
    }
    else
    {
        auto savedAudioState = settings->getXmlValue("audioDeviceState");

        // Try to load per-device-type state: extract the device type from the
        // generic key and look for a more recent type-specific key.
        if (savedAudioState)
        {
            if (auto* typeEl = savedAudioState->getChildByName("DEVICETYPE"))
            {
                auto deviceType = typeEl->getAllSubText();
                if (deviceType.isNotEmpty())
                {
                    auto perTypeKey = "audioDeviceState_" + deviceType;
                    if (auto perTypeState = settings->getXmlValue(perTypeKey))
                        savedAudioState = std::move(perTypeState);
                }
            }
        }

        // Finish audio-device discovery before making any Windows MIDI calls.
        // WinMM can hold a process-wide multimedia lock while waiting for the
        // MIDI service; probing concurrently with DirectSound discovery can
        // deadlock both operations.
        audioInitError = AudioDeviceInitHelpers::initialiseAudioWithoutMidi (
            deviceManager, 256, 256, savedAudioState.get());

        midiServiceResponsive = AudioDeviceInitHelpers::probeMidiService (std::chrono::seconds (1));

        const auto hasSavedMidiState = savedAudioState != nullptr
            && (savedAudioState->getChildByName ("MIDIINPUT") != nullptr
                || savedAudioState->hasAttribute ("defaultMidiOutput")
                || savedAudioState->hasAttribute ("defaultMidiOutputDevice"));

        // Restore MIDI selections only after the isolated probe has completed.
        // If the service timed out, keep the already-open audio device and skip
        // all MIDI restoration for this run.
        if (midiServiceResponsive && hasSavedMidiState)
            audioInitError = deviceManager.initialise (256, 256, savedAudioState.get(), false);
    }

    if (audioInitError.isNotEmpty())
        lastDeviceError = audioInitError;

    graph.setNonRealtime (hostOptions.debugMode);
    player.setProcessor(&graph);
    deviceManager.addAudioCallback(&player);

    // Register audio device error/stopped callbacks for automatic recovery
    // after sleep/wake or device disconnection.  Device callbacks may arrive
    // off the message thread, so marshal recovery work to the message thread.
    auto safeThis = Component::SafePointer<IconMenu>(this);
    player.onDeviceError = [safeThis](const String& msg) {
        MessageManager::callAsync([safeThis, msg]() {
            if (auto* self = safeThis.getComponent())
            {
                self->lastDeviceError = msg;
                self->triggerAudioDeviceRecovery();
            }
        });
    };
    player.onDeviceStopped = [safeThis]() {
        MessageManager::callAsync([safeThis]() {
            if (auto* self = safeThis.getComponent())
            {
                if (self->deviceManager.getCurrentAudioDevice() == nullptr)
                    self->triggerAudioDeviceRecovery();
            }
        });
    };
    // Plugins - all
    auto savedPluginList = getAppProperties().getUserSettings()->getXmlValue("pluginList");
    if (savedPluginList != nullptr)
        knownPluginList.recreateFromXml(*savedPluginList);
    pluginSortMethod = KnownPluginList::sortByManufacturer;
    knownPluginList.addChangeListener(this);
    pluginScanLog = std::make_shared<PluginScanLog> (
        getAppProperties().getUserSettings()->getFile().getSiblingFile ("PluginScanFailures.log"));
    knownPluginList.setCustomScanner (std::make_unique<IsolatedPluginScanner> (pluginScanLog));

    // PluginChain: unified plugin chain management
    pluginChain = std::make_unique<PluginChain>(graph, formatManager, player,
                                                 hostOptions.debugMode);
    presetManager = std::make_unique<PresetManager>(*pluginChain, getAppProperties());

    // A direct CLI plug-in request is treated as an isolated harness unless
    // --append is specified. This keeps reverse-engineering runs deterministic.
    chainPersistenceEnabled = hostOptions.plugins.empty() || hostOptions.appendPlugins;
    auto* userSettings = getAppProperties().getUserSettings();
    const auto savedPluginChain = userSettings->getXmlValue ("pluginChain");
    const bool hasSavedPluginChain = savedPluginChain != nullptr;
    if (chainPersistenceEnabled)
        if (hasSavedPluginChain)
            pluginChain->loadFromPresetXml (savedPluginChain.get());

    // An intentionally empty saved rack is valid. Only migrate the legacy
    // list when this installation has no saved chain at all; otherwise old
    // pluginListActive data would resurrect removed plugins on every launch.
    if (chainPersistenceEnabled && ! hasSavedPluginChain)
    {
        // Old format migration — use a local KnownPluginList instead of
        // a member variable, since this migration runs only once per fresh start.
        KnownPluginList oldActiveList;
        auto savedPluginListActive = getAppProperties().getUserSettings()->getXmlValue("pluginListActive");
        if (savedPluginListActive != nullptr)
            oldActiveList.recreateFromXml(*savedPluginListActive);

        for (int i = 0; i < oldActiveList.getNumTypes(); i++)
        {
            if (auto* desc = oldActiveList.getType(i))
            {
                PluginSlot slot;
                slot.desc = *desc;
                slot.bypassed = getAppProperties().getUserSettings()
                    ->getBoolValue(getPluginKey("bypass", *desc), false);
                String stateKey = getPluginKey("state", *desc);
                String stateStr = getAppProperties().getUserSettings()->getValue(stateKey);
                if (stateStr.isNotEmpty())
                    slot.state.fromBase64Encoding(stateStr);
                pluginChain->addSlot(std::move(slot));
            }
        }
        // Clean up old individual keys
        for (int i = 0; i < pluginChain->size(); i++)
        {
            auto& slot = (*pluginChain)[i];
            userSettings->removeValue(getPluginKey("state", slot.desc));
            userSettings->removeValue(getPluginKey("bypass", slot.desc));
        }
        userSettings->removeValue ("pluginListActive");
        userSettings->saveIfNeeded();
        pluginChain->saveToProperties(getAppProperties());
    }

    // Build graph from chain (pause audio, rebuild, resume)
    deviceManager.removeAudioCallback(&player);
    player.setProcessor(nullptr);
    pluginChain->loadAll();
    player.setProcessor(&graph);
    deviceManager.addAudioCallback(&player);

    setIcon();
    setIconTooltip(JUCEApplication::getInstance()->getApplicationName());
    String savedPresetPath = getAppProperties().getUserSettings()->getValue("currentPresetPath");
    if (savedPresetPath.isNotEmpty())
        presetManager->setCurrentPresetFile(File(savedPresetPath));

    // Create default preset if no preset files exist
    File presetDir = PresetManager::getDefaultPresetDirectory();
    Array<File> existingPresets = presetDir.findChildFiles(File::findFiles, false, "*.lhp");
    if (existingPresets.size() == 0)
    {
        File defaultFile = presetDir.getChildFile("default.lhp");
        presetManager->savePresetToFile(defaultFile);
        presetManager->setCurrentPresetFile(defaultFile);
        getAppProperties().getUserSettings()->setValue("currentPresetPath",
            defaultFile.getFullPathName());
        getAppProperties().saveIfNeeded();
    }

    scheduleRackAutosave();

    // Instantiate command-line plugins only after JUCE has entered its normal
    // message loop. Some plugins display modal UI during construction, and
    // loading them synchronously from JUCEApplication::initialise() can confuse
    // the macOS event loop (the JUCE AudioPluginHost uses the same deferral).
    auto startupSafeThis = Component::SafePointer<IconMenu> (this);
    MessageManager::callAsync ([startupSafeThis] {
        if (auto* self = startupSafeThis.getComponent())
        {
            self->applyStartupOptions();
            if (! self->hostOptions.exitAfterProcess
                && self->hostOptions.plugins.empty()
                && self->pluginChain->size() == 0)
                self->openMainWindow();
        }
    });
};

IconMenu::~IconMenu()
{
    pluginChain->fadeOut();
    player.suspend(deviceManager);

    // Capture the final live rack on shutdown. Keep the app's rack settings
    // and the selected preset file in sync so the next launch and the preset
    // browser both reflect the same plug-ins and state.
    if (pluginChain != nullptr && chainPersistenceEnabled)
    {
        pluginChain->saveToProperties(getAppProperties());

        if (presetManager != nullptr)
        {
            const auto currentPreset = presetManager->getCurrentPresetFile();
            if (currentPreset.existsAsFile())
                presetManager->savePresetToFile(currentPreset);
        }
    }

    PluginWindow::closeAllCurrentlyOpenWindows();
}

void IconMenu::setIcon()
{
    // Load icons via ImageCache (avoids decoding PNG on every menu open)
#if JUCE_MAC
    if (Desktop::getInstance().isDarkModeActive())
    {
        auto img = ImageCache::getFromMemory(BinaryData::menu_icon_white_png, BinaryData::menu_icon_white_pngSize);
        setIconImage(img, img);
    }
    else
    {
        auto img = ImageCache::getFromMemory(BinaryData::menu_icon_png, BinaryData::menu_icon_pngSize);
        setIconImage(img, img);
    }
#else
    String defaultColor;
#if JUCE_WINDOWS
    defaultColor = "white";
#elif JUCE_LINUX
    defaultColor = "black";
#endif
    if (!getAppProperties().getUserSettings()->containsKey("icon"))
        getAppProperties().getUserSettings()->setValue("icon", defaultColor);
    String color = getAppProperties().getUserSettings()->getValue("icon");
    Image icon;
    if (color.equalsIgnoreCase("white"))
        icon = ImageCache::getFromMemory(BinaryData::menu_icon_white_png, BinaryData::menu_icon_white_pngSize);
    else if (color.equalsIgnoreCase("black"))
        icon = ImageCache::getFromMemory(BinaryData::menu_icon_png, BinaryData::menu_icon_pngSize);
    setIconImage(icon, icon);
#endif
}

void IconMenu::changeListenerCallback(ChangeBroadcaster* changed)
{
    if (changed == &knownPluginList)
    {
        // When user clears the plugin list (count drops to 0), also clear WaveShell from blacklist
        if (knownPluginList.getNumTypes() == 0)
        {
            for (auto& b : knownPluginList.getBlacklistedFiles())
                if (b.containsIgnoreCase("WaveShell"))
                    knownPluginList.removeFromBlacklist(b);
        }

        auto savedPluginList = knownPluginList.createXml();
        if (savedPluginList != nullptr)
        {
            getAppProperties().getUserSettings()->setValue("pluginList", savedPluginList.get());
            getAppProperties().saveIfNeeded();
        }
    }
}

void IconMenu::timerCallback()
{
    stopTimer();
    menu.clear();
    menu.addSectionHeader(JUCEApplication::getInstance()->getApplicationName());
    menu.addItem (trayOpenMainItemId, "Open Light Host");
    menu.addSeparator();
    menu.addItem (1, "Quit");
    menu.addItem (2, "Reset plug-in states");
#if !JUCE_MAC
    menu.addItem (3, "Invert icon color");
#endif
#if JUCE_MAC || JUCE_LINUX
    menu.showMenuAsync(PopupMenu::Options().withTargetComponent(this), ModalCallbackFunction::forComponent(menuInvocationCallback, this));
#else
    if (x == 0 || y == 0)
    {
        POINT iconLocation;
        iconLocation.x = 0;
        iconLocation.y = 0;
        GetCursorPos(&iconLocation);
        x = iconLocation.x;
        y = iconLocation.y;
    }
    juce::Rectangle<int> rect(x, y, 1, 1);
    menu.showMenuAsync(PopupMenu::Options().withTargetScreenArea(rect), ModalCallbackFunction::forComponent(menuInvocationCallback, this));
#endif
}

void IconMenu::mouseDown(const MouseEvent& e)
{
#if JUCE_MAC || JUCE_LINUX
    Process::setDockIconVisible(true);
#endif
    if (e.mods.isRightButtonDown())
    {
        Process::makeForegroundProcess();
        menuIconLeftClicked = false;
        startTimer (50);
    }
    else
    {
        openMainWindow();
    }
}

void IconMenu::menuInvocationCallback(int id, IconMenu* im)
{
    if (id == trayOpenMainItemId) { im->openMainWindow(); return; }

    // Right click
    if ((!im->menuIconLeftClicked))
    {
        if (id == 1)
        {
            // Fade out and stop audio thread
            im->pluginChain->fadeOut();
            im->player.suspend(im->deviceManager);

            // Clear tray icon — prevents icon from persisting after exit
            {
                juce::Image clearImg(juce::Image::ARGB, 1, 1, true);
                clearImg.clear(clearImg.getBounds(), juce::Colours::transparentBlack);
                im->setIconImage(clearImg, clearImg);
            }

            if (im->chainPersistenceEnabled)
                im->pluginChain->saveToProperties(getAppProperties());
            return JUCEApplication::getInstance()->quit();
        }
        if (id == 2)
        {
            // Clear saved states and rebuild with defaults
            for (int i = 0; i < im->pluginChain->size(); i++)
                (*im->pluginChain)[i].state = MemoryBlock();

            PluginWindow::closeAllCurrentlyOpenWindows();
            im->pluginChain->fadeOut();
            im->player.suspend(im->deviceManager);
            im->pluginChain->loadAll();
            im->player.resume(im->deviceManager, im->graph);
            im->markPresetDirty();
            return;
        }
        if (id == 3)
        {
            String color = getAppProperties().getUserSettings()->getValue("icon");
            getAppProperties().getUserSettings()->setValue("icon", color.equalsIgnoreCase("black") ? "white" : "black");
            return im->setIcon();
        }
    }
#if JUCE_MAC || JUCE_LINUX
    // Click elsewhere
    if (id == 0 && !PluginWindow::containsActiveWindows())
        Process::setDockIconVisible(false);
#endif
    // Audio settings
    if (id == 1)
        im->showAudioSettings();
    // Reload
    if (id == 2)
        im->reloadPlugins();
    // Presets
    if (id == im->INDEX_PRESET_NEW)
    {
        im->presetManager->newPreset(
            [im] { im->pluginChain->fadeOut(); im->player.suspend(im->deviceManager); },
            [im] { im->player.resume(im->deviceManager, im->graph); });
        return;
    }
    if (id == im->INDEX_PRESET_SAVE)
    {
        im->saveCurrentPreset();
        return;
    }
    if (id == im->INDEX_PRESET_SAVE_AS)
    {
        FileChooser chooser("Save Preset As",
            PresetManager::getDefaultPresetDirectory().getChildFile("Untitled.lhp"),
            "*.lhp");
        if (chooser.browseForFileToSave(true))
        {
            File result = chooser.getResult();
            im->presetManager->savePresetToFile(result);
            im->presetManager->setCurrentPresetFile(result);
            getAppProperties().getUserSettings()->setValue("currentPresetPath",
                result.getFullPathName());
            getAppProperties().saveIfNeeded();
            im->presetManager->clearDirty();
            PluginWindow::updateAllTitlesAndToolbars(im);
        }
        return;
    }
    if (id == im->INDEX_PRESET_LOAD_SELECT)
    {
        FileChooser chooser("Load Preset",
            PresetManager::getDefaultPresetDirectory(), "*.lhp");
        if (chooser.browseForFileToOpen())
        {
            File result = chooser.getResult();
            im->presetManager->loadPresetFromFile(result,
                [im] { im->pluginChain->fadeOut(); im->player.suspend(im->deviceManager); },
                [im] { im->player.resume(im->deviceManager, im->graph); },
                [im]
                {
                    im->player.resumeMuted (im->deviceManager, im->graph);
                    // Let the muted audio callback retire the previous graph
                    // without processing another rack action mid-load.
                    Thread::sleep (650);
                    im->player.suspend (im->deviceManager);
                });
        }
        return;
    }
    // Debug/offline processing
    if (id == im->INDEX_DEBUG_PROCESS_ONE)
    {
        im->processDebugBlocks (1);
        return;
    }
    if (id == im->INDEX_DEBUG_PROCESS_HUNDRED)
    {
        im->processDebugBlocks (100);
        return;
    }

    // Plugins
    if (id > 2)
    {
        // Delete plugin
        if (id >= im->INDEX_DELETE && id < im->INDEX_DELETE + 1000000)
        {
            im->removePluginAt (id - im->INDEX_DELETE);
        }
        // Add plugin
        else if (im->knownPluginList.getIndexChosenByMenu(id) > -1)
        {
            PluginDescription plugin = *im->knownPluginList.getType(im->knownPluginList.getIndexChosenByMenu(id));

            im->pluginChain->add(plugin);
            im->markPresetDirty();
            PluginWindow::updateAllTitlesAndToolbars(im);
        }
        // Bypass plugin
        else if (id >= im->INDEX_BYPASS && id < im->INDEX_BYPASS + 1000000)
        {
            int index = id - im->INDEX_BYPASS;
            im->togglePluginBypass(index);
        }
        // Show / close active plugin GUI (toggle)
        else if (id >= im->INDEX_EDIT && id < im->INDEX_EDIT + 1000000)
        {
            int editIndex = id - im->INDEX_EDIT;
            if (editIndex >= 0 && editIndex < im->pluginChain->size())
            {
                auto& slot = (*im->pluginChain)[editIndex];
                if (slot.isFailed())
                {
                    NativeMessageBox::showMessageBoxAsync(MessageBoxIconType::WarningIcon,
                        "Plugin Load Failed",
                        "The plugin \"" + slot.desc.name + "\" failed to load.\n\n" + slot.errorMessage);
                }
                else if (slot.node)
                {
                    if (PluginWindow::isWindowOpenFor(slot.node->nodeID))
                    {
                        PluginWindow::closeCurrentlyOpenWindowsFor(slot.node->nodeID);
                    }
                    else
                    {
                        if (PluginWindow* const w = PluginWindow::getWindowFor(slot.node, PluginWindow::Normal, im))
                            w->forceToFront();
                    }
                }
            }
        }
        // Move plugin up the list
        else if (id >= im->INDEX_MOVE_UP && id < im->INDEX_MOVE_UP + 1000000)
        {
            int index = id - im->INDEX_MOVE_UP;

            im->pluginChain->moveUp(index);
            im->markPresetDirty();
            PluginWindow::updateAllTitlesAndToolbars(im);
        }
        // Move plugin down the list
        else if (id >= im->INDEX_MOVE_DOWN && id < im->INDEX_MOVE_DOWN + 1000000)
        {
            int index = id - im->INDEX_MOVE_DOWN;

            im->pluginChain->moveDown(index);
            im->markPresetDirty();
            PluginWindow::updateAllTitlesAndToolbars(im);
        }
    }
}

bool IconMenu::loadPluginRequest (const PluginLaunchRequest& request, String& errorMessage)
{
    File pluginFile (request.path);

    if (!pluginFile.exists())
    {
        errorMessage = "Plugin path does not exist: " + request.path;
        return false;
    }

    const String identifier = pluginFile.getFullPathName();
    std::vector<PluginDescription> candidates;

    for (int i = 0; i < formatManager.getNumFormats(); ++i)
    {
        auto* format = formatManager.getFormat (i);
        if (format == nullptr)
            continue;

        OwnedArray<PluginDescription> found;
        format->findAllTypesForFile (found, identifier);

        for (auto* desc : found)
            if (desc != nullptr)
                candidates.push_back (*desc);
    }

    if (candidates.empty())
    {
        errorMessage = "No supported plugin was found at: " + identifier;
        return false;
    }

    const PluginDescription* selected = nullptr;

    if (request.name.isNotEmpty())
    {
        for (const auto& candidate : candidates)
        {
            if (candidate.name.equalsIgnoreCase (request.name))
            {
                selected = &candidate;
                break;
            }
        }

        if (selected == nullptr)
        {
            StringArray names;
            for (const auto& candidate : candidates)
                names.addIfNotAlreadyThere (candidate.name);

            errorMessage = "Plugin name \"" + request.name + "\" was not found in " + identifier
                         + ". Available types: " + names.joinIntoString (", ");
            return false;
        }
    }
    else if (candidates.size() == 1)
    {
        selected = &candidates.front();
    }
    else
    {
        StringArray names;
        for (const auto& candidate : candidates)
            names.addIfNotAlreadyThere (candidate.name);

        errorMessage = "The plugin file contains multiple plugin types: "
                     + names.joinIntoString (", ")
                     + ". Select one with --plugin-name immediately after --plugin.";
        return false;
    }

    knownPluginList.addType (*selected);
    const int index = pluginChain->add (*selected);

    if (index < 0 || index >= pluginChain->size())
    {
        errorMessage = "Failed to add plugin to the chain: " + selected->name;
        return false;
    }

    auto& slot = (*pluginChain)[index];
    if (slot.isFailed() || slot.node == nullptr)
    {
        errorMessage = slot.errorMessage.isNotEmpty()
                     ? slot.errorMessage
                     : "Plugin instance creation failed: " + selected->name;
        pluginChain->remove (index);
        return false;
    }

    if (hostOptions.openEditors)
    {
        // The chain may change before this callback runs; resolve the node again.
        const auto nodeId = slot.node->nodeID;
        auto safeThis = Component::SafePointer<IconMenu> (this);
        MessageManager::callAsync ([safeThis, nodeId]
        {
            if (auto* self = safeThis.getComponent())
                self->openPluginEditor (self->pluginChain->getSlotIndexForNode (nodeId));
        });
    }

    return true;
}

void IconMenu::openPluginEditor (int index)
{
    if (index < 0 || index >= pluginChain->size())
        return;

    auto& slot = (*pluginChain)[index];
    if (slot.isFailed() || slot.node == nullptr)
        return;

    if (PluginWindow* const window =
            PluginWindow::getWindowFor (slot.node, PluginWindow::Normal, this))
        window->forceToFront();
}

void IconMenu::renamePluginAt (int index)
{
    if (index < 0 || index >= pluginChain->size())
        return;

    auto& slot = (*pluginChain)[index];
    AlertWindow alert ("Name plug-in instance",
                       "Give this instance a role name. The plug-in name remains visible.",
                       AlertWindow::NoIcon);
    alert.addTextEditor ("instanceLabel", slot.instanceLabel, "Role name");
    alert.addButton ("Save", 1, KeyPress (KeyPress::returnKey));
    alert.addButton ("Cancel", 0, KeyPress (KeyPress::escapeKey));

    if (alert.runModalLoop() != 1)
        return;

    if (! pluginChain->setInstanceLabel (index, alert.getTextEditorContents ("instanceLabel")))
        return;

    markPresetDirty (false);
    PluginWindow::updateAllTitlesAndToolbars (this);
    if (mainControlWindow != nullptr)
        mainControlWindow->refresh();
}

void IconMenu::openMainWindow()
{
    if (mainControlWindow == nullptr)
        mainControlWindow.reset (new MainWindow (*this));
    else
    {
        mainControlWindow->refresh();
        mainControlWindow->focus();
    }
}

void IconMenu::createNewPreset()
{
    presetManager->newPreset (
        [this] { pluginChain->fadeOut(); player.suspend (deviceManager); },
        [this] { player.resume (deviceManager, graph); });
    if (mainControlWindow != nullptr) mainControlWindow->refresh();
}

void IconMenu::loadPresetFile (const File& file)
{
    if (presetManager == nullptr || ! file.existsAsFile()) return;
    presetManager->loadPresetFromFile (file,
        [this] { pluginChain->fadeOut(); player.suspend (deviceManager); },
        [this] { player.resume (deviceManager, graph); },
        [this]
        {
            player.resumeMuted (deviceManager, graph);
            // Let the muted audio callback retire the previous graph without
            // dispatching another rack action mid-load.
            Thread::sleep (650);
            player.suspend (deviceManager);
        });
    PluginWindow::updateAllTitlesAndToolbars (this);
    if (mainControlWindow != nullptr) mainControlWindow->refresh();
}

bool IconMenu::processDebugBlocks (int blockCount, String* errorMessage)
{
    if (!hostOptions.debugMode)
    {
        if (errorMessage != nullptr)
            *errorMessage = "Manual block processing is only available in --debug mode.";
        return false;
    }

    if (blockCount <= 0)
        return true;

    auto* device = deviceManager.getCurrentAudioDevice();
    if (device == nullptr)
    {
        if (errorMessage != nullptr)
            *errorMessage = "The synthetic debug audio device is not available.";
        return false;
    }

    const int blockSize = jmax (1, device->getCurrentBufferSizeSamples());
    const int channels = jmax (2, jmax (graph.getTotalNumInputChannels(),
                                        graph.getTotalNumOutputChannels()));

    debugAudioBuffer.setSize (channels, blockSize, false, false, true);

    ScopedNoDenormals noDenormals;
    for (int i = 0; i < blockCount; ++i)
    {
        debugAudioBuffer.clear();
        debugMidiBuffer.clear();
        graph.processBlock (debugAudioBuffer, debugMidiBuffer);
    }

    return true;
}

void IconMenu::applyStartupOptions()
{
    bool startupFailed = false;

    for (const auto& request : hostOptions.plugins)
    {
        String error;
        if (!loadPluginRequest (request, error))
        {
            startupFailed = true;
            std::cerr << "Light Host: " << error.toStdString() << std::endl;

            if (!hostOptions.exitAfterProcess)
                NativeMessageBox::showMessageBoxAsync (
                    MessageBoxIconType::WarningIcon, "Plugin Load Failed", error);
        }
    }

    if (hostOptions.processBlocks > 0)
    {
        String error;
        if (!processDebugBlocks (hostOptions.processBlocks, &error))
        {
            startupFailed = true;
            std::cerr << "Light Host: " << error.toStdString() << std::endl;

            if (!hostOptions.exitAfterProcess)
                NativeMessageBox::showMessageBoxAsync (
                    MessageBoxIconType::WarningIcon, "Debug Processing Failed", error);
        }
    }

    if (hostOptions.exitAfterProcess)
    {
        MessageManager::callAsync ([startupFailed] {
            if (auto* app = JUCEApplicationBase::getInstance())
                app->setApplicationReturnValue (startupFailed ? 1 : 0);

            JUCEApplicationBase::quit();
        });
    }
}

void IconMenu::togglePluginBypass(int timeSortedIndex)
{
    pluginChain->toggleBypass(timeSortedIndex);
    markPresetDirty();
    PluginWindow::updateAllTitlesAndToolbars(this);
    if (mainControlWindow != nullptr)
    {
        Component::SafePointer<MainWindow> safeWindow (mainControlWindow.get());
        MessageManager::callAsync ([safeWindow] { if (auto* window = safeWindow.getComponent()) window->refresh(); });
    }
}

void IconMenu::movePluginUp(int timeSortedIndex)
{
    pluginChain->moveUp(timeSortedIndex);
    markPresetDirty();
    PluginWindow::updateAllTitlesAndToolbars(this);
}

void IconMenu::movePluginDown(int timeSortedIndex)
{
    pluginChain->moveDown(timeSortedIndex);
    // See comment in movePluginUp() — same reasoning applies.
    markPresetDirty();
    PluginWindow::updateAllTitlesAndToolbars(this);
}

void IconMenu::reloadPluginAt (int timeSortedIndex)
{
    if (timeSortedIndex < 0 || timeSortedIndex >= pluginChain->size())
        return;

    pluginChain->fadeOut();
    player.suspend (deviceManager);
    pluginChain->reload (timeSortedIndex);
    player.resume (deviceManager, graph);
    markPresetDirty();
    PluginWindow::updateAllTitlesAndToolbars (this);
    if (mainControlWindow != nullptr)
        mainControlWindow->refresh();
}

void IconMenu::removePluginAt (int timeSortedIndex)
{
    if (timeSortedIndex < 0 || timeSortedIndex >= pluginChain->size())
        return;

    if ((*pluginChain)[timeSortedIndex].node != nullptr)
        PluginWindow::closeCurrentlyOpenWindowsFor ((*pluginChain)[timeSortedIndex].node->nodeID);

    pluginChain->fadeOut();
    player.suspend (deviceManager);
    pluginChain->remove (timeSortedIndex);
    player.resume (deviceManager, graph);
    markPresetDirty();
    PluginWindow::updateAllTitlesAndToolbars (this);
    if (mainControlWindow != nullptr)
    {
        Component::SafePointer<MainWindow> safeWindow (mainControlWindow.get());
        MessageManager::callAsync ([safeWindow] { if (auto* window = safeWindow.getComponent()) window->refresh(); });
    }
}

void IconMenu::movePluginTo (int fromIndex, int toIndex)
{
    if (pluginChain->moveTo (fromIndex, toIndex))
    {
        markPresetDirty();
        PluginWindow::updateAllTitlesAndToolbars (this);
    if (mainControlWindow != nullptr)
        mainControlWindow->refresh();
    }
}

bool IconMenu::isBypassed(int timeSortedIndex)
{
    if (timeSortedIndex < 0 || timeSortedIndex >= pluginChain->size())
        return false;
    return (*pluginChain)[timeSortedIndex].bypassed;
}

void IconMenu::saveCurrentPreset()
{
    if (pluginChain->size() == 0)
        return;

    if (presetManager->getCurrentPresetFile().exists())
    {
        presetManager->savePresetToFile(presetManager->getCurrentPresetFile());
        presetManager->clearDirty();
        PluginWindow::updateAllTitlesAndToolbars(this);
    }
    else
    {
        saveCurrentPresetAs();
    }
}

void IconMenu::saveCurrentPresetAs()
{
    if (pluginChain->size() == 0) return;
    Process::makeForegroundProcess();
    FileChooser chooser ("Save Preset As",
        PresetManager::getDefaultPresetDirectory().getChildFile ("Untitled.lhp"), "*.lhp");
    if (! chooser.browseForFileToSave (true)) return;

    const auto file = chooser.getResult();
    presetManager->savePresetToFile (file);
    presetManager->setCurrentPresetFile (file);
    presetManager->clearDirty();
    getAppProperties().getUserSettings()->setValue ("currentPresetPath", file.getFullPathName());
    getAppProperties().saveIfNeeded();
    PluginWindow::updateAllTitlesAndToolbars (this);
    if (mainControlWindow != nullptr) mainControlWindow->refresh();
}

void IconMenu::triggerAudioDeviceRecovery()
{
    // Debounce retries and stop after the configured maximum attempt count.
    if (deviceRecentlyRecovered || deviceRecoveryRetryCount >= maxDeviceRecoveryRetries)
        return;

    deviceRecentlyRecovered = true;
    ++deviceRecoveryRetryCount;
    bool recovered = false;

    // Suspend audio callbacks
    deviceManager.removeAudioCallback (&player);
    player.setProcessor (nullptr);

    // Save the current device state XML and try to re-initialise with it
    if (auto state = deviceManager.createStateXml())
    {
        deviceManager.closeAudioDevice();
        String error = midiServiceResponsive
            ? deviceManager.initialise (256, 256, state.get(), false)
            : AudioDeviceInitHelpers::initialiseAudioWithoutMidi (deviceManager, 256, 256, state.get());

        if (error.isEmpty())
        {
            // Success — reconnect the graph and resume
            player.setProcessor (&graph);
            deviceManager.addAudioCallback (&player);
            deviceRecoveryRetryCount = 0;
            lastDeviceError.clear();
            recovered = true;

            // Persist recovered state to global
            if (auto stableState = deviceManager.createStateXml())
            {
                auto* userSettings = getAppProperties().getUserSettings();
                if (! midiServiceResponsive)
                    if (auto savedState = userSettings->getXmlValue ("audioDeviceState"))
                        AudioDeviceInitHelpers::preserveMidiSettings (savedState.get(), *stableState);

                userSettings->setValue ("audioDeviceState", stableState.get());
                getAppProperties().getUserSettings()->saveIfNeeded();
            }
        }
        else
        {
            // Still failing — leave the device closed; the UI will show the error
            lastDeviceError = error;
        }
    }
    else
    {
        // No saved state to restore — try opening a default device
        deviceManager.closeAudioDevice();
        String error = deviceManager.initialiseWithDefaultDevices (256, 256);
        if (error.isEmpty())
        {
            player.setProcessor (&graph);
            deviceManager.addAudioCallback (&player);
            deviceRecoveryRetryCount = 0;
            lastDeviceError.clear();
            recovered = true;
        }
        else
        {
            lastDeviceError = error;
        }
    }

    // Release the debounce lock after 3 seconds.  A failed recovery no longer
    // relies on another device callback arriving after callbacks were removed;
    // retry explicitly until the configured limit is reached.
    auto safeThis = Component::SafePointer<IconMenu>(this);
    Timer::callAfterDelay (3000, [safeThis, recovered]
    {
        if (auto* self = safeThis.getComponent())
        {
            self->deviceRecentlyRecovered = false;
            if (!recovered && self->deviceRecoveryRetryCount < maxDeviceRecoveryRetries)
                self->triggerAudioDeviceRecovery();
        }
    });
}

void IconMenu::scheduleRackAutosave()
{
    if (! chainPersistenceEnabled)
        return;

    auto safeThis = Component::SafePointer<IconMenu> (this);
    Timer::callAfterDelay (30000, [safeThis]
    {
        if (auto* self = safeThis.getComponent())
        {
            self->persistRackCheckpoint();
            self->scheduleRackAutosave();
        }
    });
}

void IconMenu::persistRackCheckpoint()
{
    if (chainPersistenceEnabled && pluginChain != nullptr)
        pluginChain->saveToProperties (getAppProperties());
}

void IconMenu::markPresetDirty (bool checkpointPluginState)
{
    if (checkpointPluginState)
        persistRackCheckpoint();
    else
        scheduleParameterStateCheckpoint();

    if (presetManager == nullptr)
        return;

    if (!presetManager->isDirty())
    {
        presetManager->markDirty();
        PluginWindow::updateAllTitlesAndToolbars(this);
    }
}

void IconMenu::scheduleParameterStateCheckpoint()
{
    const auto generation = ++parameterStateCheckpointGeneration;
    auto safeThis = Component::SafePointer<IconMenu> (this);
    Timer::callAfterDelay (1000, [safeThis, generation]
    {
        if (auto* self = safeThis.getComponent())
            if (self->parameterStateCheckpointGeneration == generation)
                self->persistRackCheckpoint();
    });
}


void IconMenu::showAudioSettings()
{
    openMainWindow();
    if (mainControlWindow != nullptr) mainControlWindow->showAudioTab();
}

void IconMenu::persistAudioSettings (const AudioSettingsComponent& audioSettingsComp)
{
    auto* userSettings = getAppProperties().getUserSettings();
    for (const auto& [type, state] : audioSettingsComp.getPerTypeState())
    {
        if (state == nullptr) continue;
        const auto key = "audioDeviceState_" + type;
        if (! midiServiceResponsive)
            if (auto saved = userSettings->getXmlValue (key))
                AudioDeviceInitHelpers::preserveMidiSettings (saved.get(), *state);
        userSettings->setValue (key, state.get());
    }

    if (auto stateAfter = deviceManager.createStateXml())
    {
        if (! midiServiceResponsive)
            if (auto saved = userSettings->getXmlValue ("audioDeviceState"))
                AudioDeviceInitHelpers::preserveMidiSettings (saved.get(), *stateAfter);
        userSettings->setValue ("audioDeviceState", stateAfter.get());
    }
    userSettings->setValue ("enableFade", audioSettingsComp.isFadeEnabled());
    userSettings->setValue ("defaultBpm", player.getDefaultBpm());
    userSettings->saveIfNeeded();
}

void IconMenu::refreshThemeOnOpenWindows()
{
    if (pluginListWindow != nullptr)
        pluginListWindow->applyHostTheme();
    PluginWindow::updateHostTheme();
}

void IconMenu::reloadPlugins()
{
    if (pluginListWindow == nullptr)
        pluginListWindow.reset(new PluginListWindow(*this, formatManager));
    focusWindow (*pluginListWindow);
}
