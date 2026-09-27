//
//  IconMenu.hpp
//  Light Host
//
//  Created by Rolando Islas on 12/26/15.
//
//

#ifndef IconMenu_hpp
#define IconMenu_hpp

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include "PresetManager.hpp"
#include "AudioStream.hpp"
#include "LoopbackCaptureDevice.hpp"
#include "HostOptions.hpp"
#include "IsolatedPluginScanner.hpp"

using namespace juce;

class PluginChain;
class AudioSettingsComponent;

// Emoji UTF-8 byte sequences for plugin status indicators
constexpr const char* bypassedPluginEmoji    = "\xe2\x9a\xaa"; // ⚪ U+26AA
constexpr const char* nonBypassedPluginEmoji = "\xf0\x9f\x9f\xa2"; // 🟢 U+1F7E2
constexpr const char* failedPluginEmoji      = "\xf0\x9f\x94\xb4"; // 🔴 U+1F534

ApplicationProperties& getAppProperties();

class IconMenu : public SystemTrayIconComponent, private Timer, public ChangeListener
{
public:
    explicit IconMenu (const HostOptions& options = {});
    ~IconMenu();
    void mouseDown(const MouseEvent&);
    void openMainWindow();
    static void menuInvocationCallback(int id, IconMenu*);
    void changeListenerCallback(ChangeBroadcaster* changed);

    PluginChain& getPluginChain() const { return *pluginChain; }
    PresetManager* getPresetManager() const { return presetManager.get(); }
    AudioDeviceManager& getDeviceManagerForUi() { return deviceManager; }
    AudioStream& getAudioStreamForUi() { return player; }
    String getLastDeviceError() const { return lastDeviceError; }
    bool isMidiServiceResponsive() const { return midiServiceResponsive; }

    void saveCurrentPreset();
    void saveCurrentPresetAs();
    void markPresetDirty (bool checkpointPluginState = true);
    void createNewPreset();
    void persistAudioSettings (const AudioSettingsComponent&);
    void loadPresetFile (const File& file);
    void openPluginEditor (int index);
    void renamePluginAt (int index);
    void reloadPlugins();
    void refreshThemeOnOpenWindows();

    void togglePluginBypass(int timeSortedIndex);
    void reloadPluginAt (int timeSortedIndex);
    void removePluginAt(int timeSortedIndex);
    void movePluginUp(int timeSortedIndex);
    void movePluginDown(int timeSortedIndex);
    void movePluginTo(int fromIndex, int toIndex);
    bool isBypassed(int timeSortedIndex);

    /** Process a fixed number of silent blocks on the message thread.
        Only available in --debug mode, where no real-time device callback runs. */
    bool processDebugBlocks (int blockCount, String* errorMessage = nullptr);

    const int INDEX_EDIT, INDEX_BYPASS, INDEX_DELETE, INDEX_MOVE_UP, INDEX_MOVE_DOWN;
    const int INDEX_PRESET_SAVE, INDEX_PRESET_SAVE_AS, INDEX_PRESET_LOAD_SELECT, INDEX_PRESET_NEW, INDEX_PRESET_LOAD_FILE;
    const int INDEX_DEBUG_PROCESS_ONE, INDEX_DEBUG_PROCESS_HUNDRED;

    /** @internal */
    static constexpr int maxDeviceRecoveryRetries = 5;

private:
    void timerCallback();
    void refreshMainWindow();
    void showAudioSettings();
    void setIcon();
    bool loadPluginRequest (const PluginLaunchRequest& request, String& errorMessage);
    void applyStartupOptions();
    void scheduleRackAutosave();
    void scheduleParameterStateCheckpoint();
    void persistRackCheckpoint();
    uint64_t parameterStateCheckpointGeneration = 0;

    HostOptions hostOptions;
    bool chainPersistenceEnabled = true;
    AudioDeviceManager deviceManager;
    AudioPluginFormatManager formatManager;
    KnownPluginList knownPluginList;
    std::shared_ptr<PluginScanLog> pluginScanLog;
    KnownPluginList::SortMethod pluginSortMethod;
    PopupMenu menu;
    bool menuIconLeftClicked;
    AudioProcessorGraph graph;
    AudioStream player{ deviceManager };
    AudioBuffer<float> debugAudioBuffer;
    MidiBuffer debugMidiBuffer;
    #if JUCE_WINDOWS
    int x, y;
    #endif

    std::unique_ptr<PluginChain> pluginChain;
    std::unique_ptr<PresetManager> presetManager;

    // Audio device recovery (sleep/wake, device disconnection)
    bool deviceRecentlyRecovered = false;
    int  deviceRecoveryRetryCount = 0;
    String lastDeviceError;
    bool midiServiceResponsive = true;

    void triggerAudioDeviceRecovery();

    class PluginListWindow;
    std::unique_ptr<PluginListWindow> pluginListWindow;
    class PluginRackComponent;
    class PresetBrowserComponent;
    class MainWindow;
    std::unique_ptr<MainWindow> mainControlWindow;
};

#endif /* IconMenu_hpp */
