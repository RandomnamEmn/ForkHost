#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include "AudioStream.hpp"
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class PluginRackWorkerServer;

namespace PluginRackWorkerProtocol
{
inline constexpr auto workerId = "lhrackworker";
inline constexpr uint32_t magic = 0x4c485257; // LHRW
inline constexpr uint32_t version = 6;
inline constexpr int pipelineLatencyBlocks = 3;
inline constexpr int slotCount = 16;
inline constexpr int maxChannels = 2;
inline constexpr int maxSamples = 4096;
inline constexpr int maxMidiBytes = 32768;

enum class WorkerMessage : uint32_t { initialise = 1, rack, replaceRack, openEditor, closeEditors, shutdown,
                                      format, workerReady, rackStatus, parameterState, workerHeartbeat,
                                      workerError, editorStatus };

struct AudioSlot
{
    std::atomic<int32_t> state { 0 }; // 0 free, 1 input ready, 2 output ready, 3 parent writing, 4 worker processing, 5 expired in-flight, 6 expired queued
    uint32_t sequence = 0;
    uint32_t rackGeneration = 0;
    uint64_t streamStartSample = 0;
    std::atomic<uint32_t> consumedSamples { 0 };
    std::atomic<bool> deadlineMissed { false };
    int32_t samples = 0;
    int32_t midiBytes = 0;
    int32_t midiOutputBytes = 0;
    float input[maxChannels][maxSamples] {};
    float output[maxChannels][maxSamples] {};
    uint8_t midi[maxMidiBytes] {};
    uint8_t midiOutput[maxMidiBytes] {};
};

struct SharedAudio
{
    uint32_t sharedMagic = magic;
    uint32_t sharedVersion = version;
    std::atomic<uint32_t> workerAlive { 0 };
    std::atomic<uint32_t> sampleRate { 48000 };
    std::atomic<uint32_t> blockSize { 512 };
    std::atomic<uint32_t> pipelineLatencySamples { 1536 };
    std::atomic<uint32_t> requestedRackGeneration { 0 };
    std::atomic<uint32_t> activeRackGeneration { 0 };
    std::atomic<uint32_t> completedBlocks { 0 };
    std::atomic<uint32_t> audioCallbacks { 0 };
    std::atomic<uint32_t> lastCallbackSamples { 0 };
    std::atomic<uint32_t> queuedBlocks { 0 };
    std::atomic<uint32_t> processedSamples { 0 };
    std::atomic<uint32_t> drySamples { 0 };
    std::atomic<uint32_t> startupSilenceSamples { 0 };
    std::atomic<uint32_t> missingSamples { 0 };
    std::atomic<uint32_t> changedFrames { 0 };
    std::atomic<uint32_t> loadedPlugins { 0 };
    std::atomic<uint32_t> failedPlugins { 0 };
    std::array<AudioSlot, slotCount> slots;
};

inline size_t mappingSize() noexcept { return sizeof (SharedAudio); }
}

/** AudioProcessor proxy placed in the main host's graph. Audio moves through
    a bounded shared-memory ring; the device callback never waits for the worker. */
class RemoteRackProcessor final : public juce::AudioProcessor
{
public:
    explicit RemoteRackProcessor (class PluginRackWorker&);
    const juce::String getName() const override { return "ForkHost Rack Worker"; }
    void prepareToPlay (double, int) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlock (juce::AudioBuffer<double>&, juce::MidiBuffer&) override;
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return true; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}
    void getStateInformation (juce::MemoryBlock&) override {}
    void setStateInformation (const void*, int) override {}

private:
    class PluginRackWorker& worker;
};

/** Supervises one helper for the complete rack and owns the shared audio ring. */
class PluginRackWorker final : private juce::ChildProcessCoordinator,
                               private juce::Timer
{
public:
    PluginRackWorker();
    ~PluginRackWorker() override;

    bool start (double sampleRate, int blockSize, juce::String& error);
    void setFormat (double sampleRate, int blockSize);
    void stop();
    bool isRunning() const noexcept;
    bool isConnected() const noexcept { return connected.load (std::memory_order_acquire); }
    void setRack (const juce::XmlElement&);
    void replaceRack (const juce::XmlElement&);
    void updateRestartSnapshot (const juce::XmlElement&);
    void openEditor (int slotIndex);
    void closeEditors();
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) noexcept;

    std::function<void (int, bool, const juce::String&)> onPluginStatus;
    std::function<void (int, const juce::String&)> onEditorError;
    std::function<void (int, const juce::MemoryBlock&)> onPluginState;
    std::function<void (const juce::String&)> onWorkerError;

private:
    void handleMessageFromWorker (const juce::MemoryBlock&) override;
    void handleConnectionLost() override;
    void timerCallback() override;
    bool createSharedAudioFile();
    void sendInitialise();
    bool launchHelper (juce::String& error);
    void sendRackMessage (PluginRackWorkerProtocol::WorkerMessage, const juce::String&,
                          uint32_t generation);

    juce::File sharedFile;
    std::unique_ptr<juce::MemoryMappedFile> sharedMapping;
    PluginRackWorkerProtocol::SharedAudio* shared = nullptr;
    std::atomic<bool> connected { false };
    std::atomic<bool> stopped { false };
    std::atomic<uint32_t> nextSequence { 0 };
    uint64_t nextInputSample = 0;
    int64_t nextOutputSample = 0;
    bool outputCursorStarted = false;
    float outputWetMix = 0.0f;
    std::atomic<uint32_t> rackGeneration { 0 };
    juce::String restartSnapshot;
    std::atomic<bool> restartPending { false };
    uint32_t lastObservedCompletion = 0;
    uint32_t lastProgressMs = 0;
    std::atomic<uint32_t> lastAudioCallbackMs { 0 };
    std::atomic<uint32_t> lastWorkerHeartbeatMs { 0 };
    std::atomic<uint32_t> lastRackRequestMs { 0 };
    uint32_t restartWindowStartMs = 0;
    int restartsInWindow = 0;
    uint32_t nextRestartAttemptMs = 0;
    uint32_t lastDiagnosticsMs = 0;
    juce::File diagnosticsFile;
};

/** Child-process side. Plugin instances and their editor windows live here. */
class PluginRackWorkerServer final : public juce::ChildProcessWorker,
                                     private juce::Thread,
                                     private juce::AsyncUpdater,
                                     private juce::Timer
{
public:
    PluginRackWorkerServer();
    ~PluginRackWorkerServer() override;
    bool initialiseFromCommandLine (const juce::String& commandLine);
    void handleMessageFromCoordinator (const juce::MemoryBlock&) override;
    void handleConnectionLost() override;

private:
    class StateWatcher;
    void handleAsyncUpdate() override;
    void timerCallback() override;
    void run() override;
    void setupMapping (const juce::String& path, double sampleRate, int blockSize);
    void applyRack (const juce::String& xmlText, bool forceReplace, uint32_t generation);
    void reconcileRack (const juce::String& xmlText);
    void reportRackStatus();

    juce::File sharedFile;
    std::unique_ptr<juce::MemoryMappedFile> sharedMapping;
    PluginRackWorkerProtocol::SharedAudio* shared = nullptr;
    juce::AudioProcessorGraph graph;
    juce::AudioPluginFormatManager formats;
    juce::AudioDeviceManager unusedDeviceManager;
    AudioStream* audioStream = nullptr;
    std::unique_ptr<class PluginChain> chain;
    juce::CriticalSection commandLock;
    juce::String pendingRackXml;
    juce::String pendingSharedPath;
    double pendingSampleRate = 48000.0;
    int pendingBlockSize = 512;
    uint32_t pendingRackGeneration = 0;
    uint32_t activeRackGeneration = 0;
    bool rackPending = false;
    bool replacePending = false;
    bool initialisePending = false;
    std::atomic<bool> shuttingDown { false };
    std::vector<std::unique_ptr<StateWatcher>> stateWatchers;
    std::vector<juce::Component*> editorWindows;
    std::atomic<bool> graphPaused { false };
    std::atomic<bool> processingAudio { false };
};
