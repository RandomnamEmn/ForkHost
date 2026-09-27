#include "PluginRackWorker.hpp"
#include "PluginChain.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <vector>

using namespace juce;
using namespace PluginRackWorkerProtocol;

namespace
{
constexpr int midiHeaderBytes = (int) (sizeof (int32_t) + sizeof (uint32_t));

MemoryBlock makeMessage (WorkerMessage type, const std::function<void (MemoryOutputStream&)>& write = {})
{
    MemoryBlock result;
    MemoryOutputStream stream (result, false);
    stream.writeInt ((int) type);
    if (write != nullptr)
        write (stream);
    return result;
}

bool readMessageType (WorkerMessage& type, MemoryInputStream& stream)
{
    stream.setPosition (0);
    const auto rawType = stream.readInt();
    if (rawType < (int) WorkerMessage::initialise || rawType > (int) WorkerMessage::editorStatus)
        return false;
    type = (WorkerMessage) rawType;
    return true;
}

void appendMidi (MidiBuffer& midi, const uint8_t* bytes, int numBytes)
{
    int offset = 0;
    while (offset + midiHeaderBytes <= numBytes)
    {
        int32_t samplePosition = 0;
        uint32_t messageBytes = 0;
        std::memcpy (&samplePosition, bytes + offset, sizeof (samplePosition));
        offset += (int) sizeof (samplePosition);
        std::memcpy (&messageBytes, bytes + offset, sizeof (messageBytes));
        offset += (int) sizeof (messageBytes);

        if (messageBytes == 0 || messageBytes > (uint32_t) (numBytes - offset))
            return;

        midi.addEvent (bytes + offset, (int) messageBytes, samplePosition);
        offset += (int) messageBytes;
    }
}

void appendMidiRange (MidiBuffer& destination, const uint8_t* bytes, int numBytes,
                      uint64_t sourceStart, uint64_t outputStart, int outputSamples)
{
    int offset = 0;
    const auto outputEnd = outputStart + (uint64_t) outputSamples;
    while (offset + midiHeaderBytes <= numBytes)
    {
        int32_t samplePosition = 0;
        uint32_t messageBytes = 0;
        std::memcpy (&samplePosition, bytes + offset, sizeof (samplePosition));
        offset += (int) sizeof (samplePosition);
        std::memcpy (&messageBytes, bytes + offset, sizeof (messageBytes));
        offset += (int) sizeof (messageBytes);
        if (messageBytes == 0 || messageBytes > (uint32_t) (numBytes - offset))
            return;

        const auto eventSample = sourceStart + (uint64_t) jmax (0, samplePosition);
        if (eventSample >= outputStart && eventSample < outputEnd)
            destination.addEvent (bytes + offset, (int) messageBytes,
                                  (int) (eventSample - outputStart));
        offset += (int) messageBytes;
    }
}

void writeMidi (const MidiBuffer& midi, uint8_t* destination, int capacity, int& written)
{
    written = 0;
    MidiBuffer::Iterator iterator (midi);
    const uint8_t* messageData = nullptr;
    int messageBytes = 0;
    int samplePosition = 0;

    while (iterator.getNextEvent (messageData, messageBytes, samplePosition))
    {
        const auto required = midiHeaderBytes + messageBytes;
        if (messageBytes <= 0 || required > capacity - written)
            break;

        const auto eventPosition = (int32_t) samplePosition;
        const auto eventSize = (uint32_t) messageBytes;
        std::memcpy (destination + written, &eventPosition, sizeof (eventPosition));
        written += (int) sizeof (eventPosition);
        std::memcpy (destination + written, &eventSize, sizeof (eventSize));
        written += (int) sizeof (eventSize);
        std::memcpy (destination + written, messageData, (size_t) messageBytes);
        written += messageBytes;
    }
}

bool ensureMappedFileSize (const File& file, size_t bytes)
{
    FileOutputStream stream (file);
    if (! stream.openedOk() || bytes == 0)
        return false;

    stream.setPosition ((int64) bytes - 1);
    stream.writeByte (0);
    stream.flush();
    return stream.getStatus().wasOk();
}

String getSlotIdentity (const PluginSlot& slot)
{
    return slot.desc.pluginFormatName + "|" + slot.desc.fileOrIdentifier + "|"
         + String (slot.desc.uniqueId) + "|" + String (slot.instanceSequence);
}

class WorkerEditorWindow final : public DocumentWindow
{
public:
    WorkerEditorWindow (AudioProcessorGraph::Node::Ptr node, Component* editor,
                        int index, String identity)
        : DocumentWindow (node->getProcessor()->getName(), Colours::darkgrey,
                          DocumentWindow::allButtons), owner (std::move (node)),
          slotIndex (index), instanceIdentity (std::move (identity))
    {
        setUsingNativeTitleBar (true);
        setResizable (true, true);
        setContentOwned (editor, true);
        addToDesktop (0);
        centreAroundComponent (nullptr, getWidth(), getHeight());
        setVisible (true);
        toFront (true);
    }

    int getSlotIndex() const noexcept { return slotIndex; }
    const String& getInstanceIdentity() const noexcept { return instanceIdentity; }
    void setSlotIndex (int newIndex)
    {
        slotIndex = newIndex;
        setName ("[" + String (slotIndex + 1) + "] " + owner->getProcessor()->getName());
    }
    void closeButtonPressed() override { setVisible (false); }

private:
    AudioProcessorGraph::Node::Ptr owner;
    int slotIndex = -1;
    String instanceIdentity;
};

}

class PluginRackWorkerServer::StateWatcher final : public AudioProcessorListener
{
public:
    StateWatcher (AudioProcessor& processorToWatch, int slot)
        : processor (&processorToWatch), index (slot)
    {
        processor->addListener (this);
    }

    ~StateWatcher() override
    {
        if (processor != nullptr)
            processor->removeListener (this);
    }

    void audioProcessorParameterChanged (AudioProcessor*, int, float) override { markChanged(); }
    void audioProcessorChanged (AudioProcessor*, const ChangeDetails&) override { markChanged(); }

    void markChanged() noexcept
    {
        changedAt.store (Time::getMillisecondCounter(), std::memory_order_relaxed);
        dirty.store (true, std::memory_order_release);
    }

    AudioProcessor* processor = nullptr;
    int index = -1;
    std::atomic<uint32_t> changedAt { 0 };
    std::atomic<bool> dirty { false };
};

//==============================================================================
RemoteRackProcessor::RemoteRackProcessor (PluginRackWorker& workerIn)
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  AudioChannelSet::stereo(), true)
                          .withOutput ("Output", AudioChannelSet::stereo(), true)),
      worker (workerIn)
{
}

void RemoteRackProcessor::prepareToPlay (double sampleRate, int blockSize)
{
    worker.setFormat (sampleRate, blockSize);
    setLatencySamples (blockSize * pipelineLatencyBlocks);
}

bool RemoteRackProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto input = layouts.getMainInputChannelSet();
    const auto output = layouts.getMainOutputChannelSet();
    return (input == AudioChannelSet::mono() || input == AudioChannelSet::stereo())
        && (output == AudioChannelSet::mono() || output == AudioChannelSet::stereo());
}

void RemoteRackProcessor::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi)
{
    ScopedNoDenormals noDenormals;
    worker.processBlock (buffer, midi);
}

void RemoteRackProcessor::processBlock (AudioBuffer<double>& buffer, MidiBuffer& midi)
{
    buffer.clear();
    midi.clear();
    ignoreUnused (worker);
}

//==============================================================================
PluginRackWorker::PluginRackWorker() = default;

PluginRackWorker::~PluginRackWorker()
{
    stopTimer();
    stop();
}

bool PluginRackWorker::createSharedAudioFile()
{
    sharedFile = File::getSpecialLocation (File::tempDirectory)
                     .getChildFile ("ForkHostRack-"
                                    + String::toHexString (Random::getSystemRandom().nextInt64())
                                    + ".shared");

    if (! ensureMappedFileSize (sharedFile, mappingSize()))
        return false;

    sharedMapping = std::make_unique<MemoryMappedFile> (sharedFile, MemoryMappedFile::readWrite);
    if (sharedMapping->getData() == nullptr || sharedMapping->getSize() < mappingSize())
        return false;

    shared = new (sharedMapping->getData()) SharedAudio();
    return true;
}

bool PluginRackWorker::start (double sampleRate, int blockSize, String& error)
{
    stopped.store (false, std::memory_order_release);
    if (isRunning())
        return true;

    if (shared == nullptr && ! createSharedAudioFile())
    {
        error = "Could not create shared audio memory for the plug-in worker.";
        return false;
    }

    shared->sampleRate.store ((uint32_t) jlimit (8000.0, 768000.0, sampleRate));
    shared->blockSize.store (jlimit (16, maxSamples, blockSize));
    shared->pipelineLatencySamples.store ((uint32_t) (jlimit (16, maxSamples, blockSize)
                                                       * pipelineLatencyBlocks));
    if (! isTimerRunning())
        startTimer (250);

    return launchHelper (error);
}

bool PluginRackWorker::launchHelper (String& error)
{
    if (shared == nullptr)
    {
        error = "Shared audio memory is unavailable.";
        return false;
    }

    for (auto& slot : shared->slots)
    {
        const auto state = slot.state.load (std::memory_order_acquire);
        if (state != 0 && state != 3)
            slot.state.store (0, std::memory_order_release);
    }
    shared->workerAlive.store (0, std::memory_order_release);
    // Keep the host in its dry/pass-through state until the new helper has
    // reconstructed the requested rack snapshot.
    shared->activeRackGeneration.store (0, std::memory_order_release);

    if (! launchWorkerProcess (File::getSpecialLocation (File::currentExecutableFile), workerId, 10000,
                               ChildProcess::wantStdOut | ChildProcess::wantStdErr))
    {
        connected.store (false, std::memory_order_release);
        error = "Could not start the plug-in worker process.";
        return false;
    }

    connected.store (true, std::memory_order_release);
    restartPending.store (false, std::memory_order_release);
    lastProgressMs = Time::getMillisecondCounter();
    lastWorkerHeartbeatMs.store (lastProgressMs, std::memory_order_release);
    sendInitialise();
    const auto generation = rackGeneration.load (std::memory_order_acquire);
    const auto snapshot = restartSnapshot;
    if (snapshot.isNotEmpty())
        sendRackMessage (WorkerMessage::replaceRack, snapshot, generation);
    return true;
}

void PluginRackWorker::sendInitialise()
{
    if (sharedFile == File())
        return;

    const auto message = makeMessage (WorkerMessage::initialise, [this] (MemoryOutputStream& stream)
    {
        stream.writeString (sharedFile.getFullPathName());
        stream.writeDouble ((double) shared->sampleRate.load());
        stream.writeInt ((int) shared->blockSize.load());
    });
    sendMessageToWorker (message);
}

void PluginRackWorker::stop()
{
    stopped.store (true, std::memory_order_release);
    connected.store (false, std::memory_order_release);
    restartPending.store (false, std::memory_order_release);
    if (shared != nullptr)
        shared->workerAlive.store (0, std::memory_order_release);
    killWorkerProcess();
    shared = nullptr;
    sharedMapping.reset();
    if (sharedFile != File())
    {
        sharedFile.deleteFile();
        sharedFile = {};
    }
}

bool PluginRackWorker::isRunning() const noexcept
{
    return connected.load (std::memory_order_acquire)
        && shared != nullptr
        && shared->workerAlive.load (std::memory_order_acquire) != 0;
}

void PluginRackWorker::setRack (const XmlElement& xml)
{
    const auto text = xml.toString (XmlElement::TextFormat().singleLine());
    restartSnapshot = text;
    const auto generation = rackGeneration.fetch_add (1, std::memory_order_acq_rel) + 1;
    if (shared != nullptr)
        shared->requestedRackGeneration.store (generation, std::memory_order_release);
    lastRackRequestMs.store (Time::getMillisecondCounter(), std::memory_order_release);
    sendRackMessage (WorkerMessage::rack, text, generation);
}

void PluginRackWorker::replaceRack (const XmlElement& xml)
{
    const auto text = xml.toString (XmlElement::TextFormat().singleLine());
    restartSnapshot = text;
    const auto generation = rackGeneration.fetch_add (1, std::memory_order_acq_rel) + 1;
    if (shared != nullptr)
        shared->requestedRackGeneration.store (generation, std::memory_order_release);
    lastRackRequestMs.store (Time::getMillisecondCounter(), std::memory_order_release);
    sendRackMessage (WorkerMessage::replaceRack, text, generation);
}

void PluginRackWorker::updateRestartSnapshot (const XmlElement& xml)
{
    restartSnapshot = xml.toString (XmlElement::TextFormat().singleLine());
}

void PluginRackWorker::sendRackMessage (WorkerMessage type, const String& text, uint32_t generation)
{
    const auto message = makeMessage (type, [&text, generation] (MemoryOutputStream& stream)
    {
        stream.writeInt64 ((int64) generation);
        stream.writeString (text);
    });
    if (connected.load (std::memory_order_acquire))
        sendMessageToWorker (message);
}

void PluginRackWorker::openEditor (int slotIndex)
{
    if (! connected.load (std::memory_order_acquire))
    {
        if (onEditorError != nullptr)
            onEditorError (slotIndex, "The plug-in helper is not connected.");
        return;
    }

    const auto message = makeMessage (WorkerMessage::openEditor, [slotIndex] (MemoryOutputStream& stream)
    {
        stream.writeInt (slotIndex);
    });
    if (connected.load (std::memory_order_acquire))
        sendMessageToWorker (message);
}

void PluginRackWorker::closeEditors()
{
    if (connected.load (std::memory_order_acquire))
        sendMessageToWorker (makeMessage (WorkerMessage::closeEditors));
}

void PluginRackWorker::setFormat (double sampleRate, int blockSize)
{
    if (shared == nullptr)
        return;

    shared->sampleRate.store ((uint32_t) jmax (1.0, sampleRate), std::memory_order_release);
    shared->blockSize.store (jlimit (1, maxSamples, blockSize), std::memory_order_release);
    shared->pipelineLatencySamples.store ((uint32_t) (jlimit (1, maxSamples, blockSize)
                                                       * pipelineLatencyBlocks),
                                          std::memory_order_release);
    const auto message = makeMessage (WorkerMessage::format, [sampleRate, blockSize] (MemoryOutputStream& stream)
    {
        stream.writeDouble (sampleRate);
        stream.writeInt (blockSize);
    });
    if (connected.load (std::memory_order_acquire))
        sendMessageToWorker (message);
}

void PluginRackWorker::timerCallback()
{
    if (shared == nullptr || stopped.load (std::memory_order_acquire))
        return;

    const auto now = Time::getMillisecondCounter();
    const auto alive = shared->workerAlive.load (std::memory_order_acquire) != 0;
    const auto callbacksRecent = (uint32_t) (now - lastAudioCallbackMs.load (std::memory_order_acquire)) < 1000;
    const auto requested = shared->requestedRackGeneration.load (std::memory_order_acquire);
    const auto active = shared->activeRackGeneration.load (std::memory_order_acquire);
    const auto completed = shared->completedBlocks.load (std::memory_order_acquire);

    if (lastDiagnosticsMs == 0 || (uint32_t) (now - lastDiagnosticsMs) >= 2000)
    {
        auto directory = File::getSpecialLocation (File::userApplicationDataDirectory).getChildFile ("ForkHost");
        directory.createDirectory();
        diagnosticsFile = directory.getChildFile ("PluginRackWorker.log");
        if (diagnosticsFile.getSize() > 512 * 1024)
            diagnosticsFile.deleteFile();

        FileOutputStream log (diagnosticsFile);
        if (log.openedOk())
        {
            log.setPosition (diagnosticsFile.getSize());
            const auto line = Time::getCurrentTime().toISO8601 (true)
                + " callbacks=" + String ((int) shared->audioCallbacks.exchange (0, std::memory_order_acq_rel))
                + " callbackSamples=" + String ((int) shared->lastCallbackSamples.load (std::memory_order_acquire))
                + " queued=" + String ((int) shared->queuedBlocks.exchange (0, std::memory_order_acq_rel))
                + " completed=" + String ((int) completed)
                + " processedSamples=" + String ((int) shared->processedSamples.exchange (0, std::memory_order_acq_rel))
                + " drySamples=" + String ((int) shared->drySamples.exchange (0, std::memory_order_acq_rel))
                + " startupSilenceSamples=" + String ((int) shared->startupSilenceSamples.exchange (0, std::memory_order_acq_rel))
                + " missingSamples=" + String ((int) shared->missingSamples.exchange (0, std::memory_order_acq_rel))
                + " changedFrames=" + String ((int) shared->changedFrames.exchange (0, std::memory_order_acq_rel))
                + " loadedPlugins=" + String ((int) shared->loadedPlugins.load (std::memory_order_acquire))
                + " failedPlugins=" + String ((int) shared->failedPlugins.load (std::memory_order_acquire))
                + " rate=" + String ((int) shared->sampleRate.load (std::memory_order_acquire))
                + " block=" + String ((int) shared->blockSize.load (std::memory_order_acquire))
                + " latency=" + String ((int) shared->pipelineLatencySamples.load (std::memory_order_acquire))
                + " alive=" + String (alive ? 1 : 0)
                + " rack=" + String ((int) active) + "/" + String ((int) requested) + "\n";
            log.writeText (line, false, false, nullptr);
            log.flush();
        }
        lastDiagnosticsMs = now;
    }

    if (completed != lastObservedCompletion)
    {
        lastObservedCompletion = completed;
        lastProgressMs = now;
    }

    const auto rackRequest = lastRackRequestMs.load (std::memory_order_acquire);
    const bool rackLoadTimedOut = requested != active && rackRequest != 0
        && (uint32_t) (now - rackRequest) > 30000;
    const bool audioStalled = callbacksRecent && alive && requested == active
        && lastProgressMs != 0 && (uint32_t) (now - lastProgressMs) > 2500;
    const auto heartbeat = lastWorkerHeartbeatMs.load (std::memory_order_acquire);
    const bool controlThreadStalled = callbacksRecent && alive && requested == active
        && heartbeat != 0 && (uint32_t) (now - heartbeat) > 2500;
    const bool processExited = connected.load (std::memory_order_acquire)
        && ! isRunning() && lastProgressMs != 0
        && (uint32_t) (now - lastProgressMs) > 5000
        && ! restartPending.load (std::memory_order_acquire);

    if (rackLoadTimedOut || audioStalled || controlThreadStalled || processExited)
    {
        connected.store (false, std::memory_order_release);
        shared->workerAlive.store (0, std::memory_order_release);
        killWorkerProcess();
        restartPending.store (true, std::memory_order_release);
        if (onWorkerError != nullptr)
            onWorkerError (rackLoadTimedOut ? "A plug-in took too long to load. Restarting the rack worker."
            : audioStalled ? "Plug-in audio processing stopped responding. Restarting the rack worker."
            : controlThreadStalled ? "The plug-in worker stopped responding. Restarting the rack worker."
                                   : "The rack worker exited. Restarting it.");
    }

    if (! restartPending.load (std::memory_order_acquire))
        return;

    if (nextRestartAttemptMs != 0 && (int32_t) (now - nextRestartAttemptMs) < 0)
        return;

    if (restartWindowStartMs == 0 || (uint32_t) (now - restartWindowStartMs) > 60000)
    {
        restartWindowStartMs = now;
        restartsInWindow = 0;
    }

    if (restartsInWindow >= 3)
    {
        restartPending.store (false, std::memory_order_release);
        if (onWorkerError != nullptr)
            onWorkerError ("The plug-in worker stopped repeatedly. Audio will continue without plug-ins until ForkHost restarts.");
        return;
    }

    ++restartsInWindow;
    nextRestartAttemptMs = now + 2000;
    String error;
    if (launchHelper (error))
    {
        lastProgressMs = now;
        lastObservedCompletion = shared->completedBlocks.load (std::memory_order_acquire);
    }
    else
    {
        restartPending.store (true, std::memory_order_release);
        if (onWorkerError != nullptr)
            onWorkerError (error);
    }
}

void PluginRackWorker::processBlock (AudioBuffer<float>& buffer, MidiBuffer& midi) noexcept
{
    lastAudioCallbackMs.store (Time::getMillisecondCounter(), std::memory_order_release);
    const auto numSamples = buffer.getNumSamples();
    const auto numChannels = jmin (buffer.getNumChannels(), maxChannels);
    if (numSamples <= 0 || numSamples > maxSamples || shared == nullptr)
        return;
    shared->audioCallbacks.fetch_add (1, std::memory_order_relaxed);
    shared->lastCallbackSamples.store ((uint32_t) numSamples, std::memory_order_relaxed);

    const auto sequence = nextSequence.fetch_add (1, std::memory_order_relaxed);
    const auto inputStart = nextInputSample;
    nextInputSample += (uint64_t) numSamples;
    const auto requestedGeneration = shared->requestedRackGeneration.load (std::memory_order_acquire);
    const auto activeGeneration = shared->activeRackGeneration.load (std::memory_order_acquire);
    const auto latencySamples = shared->pipelineLatencySamples.load (std::memory_order_acquire);

    const bool canProcess = connected.load (std::memory_order_acquire)
        && requestedGeneration == activeGeneration
        && shared->workerAlive.load (std::memory_order_acquire) != 0;
    if (! outputCursorStarted)
    {
        nextOutputSample = - (int64_t) latencySamples;
        outputCursorStarted = true;
    }
    const bool hasTarget = nextOutputSample >= 0;
    const auto targetStart = nextOutputSample;

    // Retire slots whose samples are older than the output cursor. A helper
    // still processing such a slot is marked expired and discards its result.
    if (targetStart > 0)
        for (auto& slot : shared->slots)
        {
            const auto state = slot.state.load (std::memory_order_acquire);
            const auto slotEnd = (int64_t) slot.streamStartSample + jmax (0, slot.samples);
            if (state != 0 && state != 3 && slotEnd <= targetStart)
            {
                if (state == 1 || state == 2 || state == 6)
                {
                    int stale = state;
                    slot.state.compare_exchange_strong (stale, 0, std::memory_order_acq_rel);
                }
                else if (state == 4)
                {
                    slot.deadlineMissed.store (true, std::memory_order_release);
                    int processing = 4;
                    slot.state.compare_exchange_strong (processing, 5, std::memory_order_acq_rel);
                }
            }
        }

    // Queue this block before consuming old output, preserving the current
    // device input even when a worker result is ready at the same time.
    {
        auto& destination = shared->slots[sequence % slotCount];
        int expected = 0;
        if (destination.state.compare_exchange_strong (expected, 3, std::memory_order_acq_rel))
        {
            destination.sequence = sequence;
            destination.rackGeneration = requestedGeneration;
            destination.streamStartSample = inputStart;
            destination.consumedSamples.store (0, std::memory_order_relaxed);
            destination.deadlineMissed.store (false, std::memory_order_relaxed);
            destination.samples = numSamples;
            destination.midiOutputBytes = 0;
            for (int channel = 0; channel < maxChannels; ++channel)
            {
                if (channel < numChannels)
                    FloatVectorOperations::copy (destination.input[channel], buffer.getReadPointer (channel), numSamples);
                else
                    FloatVectorOperations::clear (destination.input[channel], numSamples);
            }
            writeMidi (midi, destination.midi, maxMidiBytes, destination.midiBytes);
            destination.state.store (1, std::memory_order_release);
            shared->queuedBlocks.fetch_add (1, std::memory_order_relaxed);
        }

        midi.clear();
        if (hasTarget)
        {
            auto outputCursor = targetStart;
            int outputOffset = 0;
            while (outputOffset < numSamples)
            {
                AudioSlot* source = nullptr;
                int sourceState = 0;
                for (auto& slot : shared->slots)
                {
                    const auto state = slot.state.load (std::memory_order_acquire);
                    if (state == 0 || state == 3)
                        continue;

                    const auto start = (int64_t) slot.streamStartSample;
                    const auto end = start + jmax (0, slot.samples);
                    if (outputCursor >= start && outputCursor < end)
                    {
                        source = &slot;
                        sourceState = state;
                        break;
                    }
                }

                if (source == nullptr)
                {
                    const auto missing = 1;
                    for (int channel = 0; channel < numChannels; ++channel)
                        buffer.getWritePointer (channel)[outputOffset] = 0.0f;
                    outputWetMix = jmax (0.0f, outputWetMix - 1.0f / 128.0f);
                    shared->missingSamples.fetch_add ((uint32_t) missing, std::memory_order_relaxed);
                    outputOffset += missing;
                    ++outputCursor;
                    continue;
                }

                const auto sourceStart = (int64_t) source->streamStartSample;
                const auto sourceEnd = sourceStart + source->samples;
                const auto segmentSamples = jmin (numSamples - outputOffset,
                    (int) jmin ((int64_t) std::numeric_limits<int>::max(), sourceEnd - outputCursor));
                if (segmentSamples <= 0)
                    break;

                const auto sourceOffset = (int) (outputCursor - sourceStart);
                const bool useProcessed = canProcess && sourceState == 2
                    && source->rackGeneration == activeGeneration
                    && activeGeneration == requestedGeneration
                    && ! source->deadlineMissed.load (std::memory_order_acquire);
                (useProcessed ? shared->processedSamples : shared->drySamples)
                    .fetch_add ((uint32_t) segmentSamples, std::memory_order_relaxed);
                for (int i = 0; i < segmentSamples; ++i)
                {
                    const auto targetMix = useProcessed ? 1.0f : 0.0f;
                    if (outputWetMix < targetMix) outputWetMix = jmin (targetMix, outputWetMix + 1.0f / 128.0f);
                    else if (outputWetMix > targetMix) outputWetMix = jmax (targetMix, outputWetMix - 1.0f / 128.0f);
                    for (int channel = 0; channel < numChannels; ++channel)
                    {
                        const auto dry = source->input[channel][sourceOffset + i];
                        const auto wet = source->output[channel][sourceOffset + i];
                        buffer.getWritePointer (channel)[outputOffset + i] = dry + (wet - dry) * outputWetMix;
                    }
                }

                appendMidiRange (midi,
                    useProcessed ? source->midiOutput : source->midi,
                    useProcessed ? source->midiOutputBytes : source->midiBytes,
                    (uint64_t) sourceStart, (uint64_t) outputCursor, segmentSamples);

                if (! useProcessed)
                {
                    source->deadlineMissed.store (true, std::memory_order_release);
                    int pending = sourceState;
                    if (sourceState == 1)
                        source->state.compare_exchange_strong (pending, 6, std::memory_order_acq_rel);
                    else if (sourceState == 4)
                        source->state.compare_exchange_strong (pending, 5, std::memory_order_acq_rel);
                }

                const auto consumed = source->consumedSamples.fetch_add ((uint32_t) segmentSamples,
                                                                          std::memory_order_acq_rel)
                                    + (uint32_t) segmentSamples;
                outputOffset += segmentSamples;
                outputCursor += segmentSamples;

                if (consumed >= (uint32_t) source->samples)
                {
                    auto state = source->state.load (std::memory_order_acquire);
                    if (state == 1)
                    {
                        source->deadlineMissed.store (true, std::memory_order_release);
                        int pending = 1;
                        source->state.compare_exchange_strong (pending, 0, std::memory_order_acq_rel);
                    }
                    else if (state == 2 || state == 6)
                    {
                        source->state.compare_exchange_strong (state, 0, std::memory_order_acq_rel);
                    }
                    else if (state == 4)
                    {
                        source->deadlineMissed.store (true, std::memory_order_release);
                        int processing = 4;
                        source->state.compare_exchange_strong (processing, 5, std::memory_order_acq_rel);
                    }
                }
            }

            // Keep the source timeline continuous if a device changes its
            // callback size. Recomputing inputStart - latency each callback
            // made the cursor jump by three times the buffer-size change,
            // skipping or replaying audio samples.
        }
    }
    if (! hasTarget)
    {
        if (targetStart < 0)
        {
            const auto silence = (int) jmin ((int64_t) numSamples, -targetStart);
            for (int channel = 0; channel < numChannels; ++channel)
                FloatVectorOperations::clear (buffer.getWritePointer (channel), silence);
            shared->startupSilenceSamples.fetch_add ((uint32_t) silence, std::memory_order_relaxed);
        }
        for (auto& slot : shared->slots)
        {
            const auto state = slot.state.load (std::memory_order_acquire);
            if ((state == 1 || state == 2) && slot.rackGeneration != requestedGeneration)
            {
                int stale = state;
                slot.state.compare_exchange_strong (stale, 0, std::memory_order_acq_rel);
            }
        }
    }
    // Advance the delayed playback cursor for every device callback, even
    // before the worker becomes ready or while it is reconnecting.
    nextOutputSample += (int64_t) numSamples;
}

void PluginRackWorker::handleMessageFromWorker (const MemoryBlock& message)
{
    MemoryInputStream stream (message, false);
    WorkerMessage type {};
    if (! readMessageType (type, stream))
        return;

    if (type == WorkerMessage::workerReady)
    {
        lastWorkerHeartbeatMs.store (Time::getMillisecondCounter(), std::memory_order_release);
        if (shared != nullptr)
            shared->workerAlive.store (1, std::memory_order_release);
        return;
    }

    if (type == WorkerMessage::workerHeartbeat)
    {
        lastWorkerHeartbeatMs.store (Time::getMillisecondCounter(), std::memory_order_release);
        return;
    }

    if (type == WorkerMessage::workerError)
    {
        if (onWorkerError != nullptr)
            onWorkerError (stream.readString());
        return;
    }

    if (type == WorkerMessage::editorStatus)
    {
        const auto index = stream.readInt();
        const auto opened = stream.readBool();
        const auto error = stream.readString();
        if (! opened && onEditorError != nullptr)
            onEditorError (index, error);
        return;
    }

    if (type == WorkerMessage::rackStatus)
    {
        const auto generation = (uint32_t) stream.readInt64();
        if (shared == nullptr
            || generation != shared->requestedRackGeneration.load (std::memory_order_acquire)
            || generation != shared->activeRackGeneration.load (std::memory_order_acquire))
            return;
        const auto count = jlimit (0, 4096, stream.readInt());
        for (int i = 0; i < count; ++i)
        {
            const auto index = stream.readInt();
            const auto ok = stream.readBool();
            const auto error = stream.readString();
            if (onPluginStatus != nullptr)
                onPluginStatus (index, ok, error);
        }
        return;
    }

    if (type == WorkerMessage::parameterState)
    {
        const auto generation = (uint32_t) stream.readInt64();
        if (shared == nullptr
            || generation != shared->requestedRackGeneration.load (std::memory_order_acquire)
            || generation != shared->activeRackGeneration.load (std::memory_order_acquire))
            return;
        const auto index = stream.readInt();
        const auto size = jlimit (0, 64 * 1024 * 1024, stream.readInt());
        if (size > 0 && stream.getNumBytesRemaining() >= size)
        {
            MemoryBlock state (nullptr, (size_t) size);
            stream.read (state.getData(), (size_t) size);
            if (onPluginState != nullptr)
                onPluginState (index, state);
        }
    }
}

void PluginRackWorker::handleConnectionLost()
{
    connected.store (false, std::memory_order_release);
    if (stopped.load (std::memory_order_acquire))
        return;
    restartPending.store (true, std::memory_order_release);
    if (shared != nullptr)
        shared->workerAlive.store (0, std::memory_order_release);
    if (onWorkerError != nullptr)
        onWorkerError ("The plug-in worker disconnected.");
}

//==============================================================================
PluginRackWorkerServer::PluginRackWorkerServer()
    : Thread ("ForkHost rack worker")
{
    formats.addDefaultFormats();
    audioStream = new AudioStream (unusedDeviceManager);
    chain = std::make_unique<PluginChain> (graph, formats, *audioStream, false);
    startTimer (150);
}

PluginRackWorkerServer::~PluginRackWorkerServer()
{
    shuttingDown.store (true, std::memory_order_release);
    cancelPendingUpdate();
    stopTimer();
    stopThread (2000);
    // Capture only instances with unsettled parameter edits before their
    // watchers are replaced. This preserves the quiet-period save behavior
    // without serializing every plug-in on each rack metadata change.
    for (auto& watcher : stateWatchers)
    {
        if (! watcher->dirty.load (std::memory_order_acquire))
            continue;

        const auto index = watcher->index;
        if (index < 0 || index >= chain->size() || (*chain)[index].node == nullptr
            || watcher->processor != (*chain)[index].node->getProcessor())
            continue;

        MemoryBlock state;
        watcher->processor->getStateInformation (state);
        (*chain)[index].state = std::move (state);
    }
    stateWatchers.clear();
    if (chain != nullptr)
        chain->clear();
    chain.reset();
    delete audioStream;
    audioStream = nullptr;
    shared = nullptr;
    sharedMapping.reset();
}

bool PluginRackWorkerServer::initialiseFromCommandLine (const String& commandLine)
{
    return ChildProcessWorker::initialiseFromCommandLine (commandLine, workerId, 0);
}

void PluginRackWorkerServer::handleMessageFromCoordinator (const MemoryBlock& message)
{
    MemoryInputStream stream (message, false);
    WorkerMessage type {};
    if (! readMessageType (type, stream))
        return;

    if (type == WorkerMessage::initialise)
    {
        const auto path = stream.readString();
        const auto sampleRate = stream.readDouble();
        const auto blockSize = stream.readInt();
        {
            const ScopedLock lock (commandLock);
            pendingSharedPath = path;
            pendingSampleRate = sampleRate;
            pendingBlockSize = blockSize;
            initialisePending = true;
        }
        triggerAsyncUpdate();
        return;
    }

    if (type == WorkerMessage::rack)
    {
        {
            const ScopedLock lock (commandLock);
            pendingRackGeneration = (uint32_t) stream.readInt64();
            pendingRackXml = stream.readString();
            rackPending = true;
        }
        triggerAsyncUpdate();
        return;
    }

    if (type == WorkerMessage::replaceRack)
    {
        {
            const ScopedLock lock (commandLock);
            pendingRackGeneration = (uint32_t) stream.readInt64();
            pendingRackXml = stream.readString();
            replacePending = true;
        }
        triggerAsyncUpdate();
        return;
    }

    if (type == WorkerMessage::format)
    {
        const auto sampleRate = stream.readDouble();
        const auto blockSize = stream.readInt();
        {
            const ScopedLock lock (commandLock);
            pendingSampleRate = sampleRate;
            pendingBlockSize = blockSize;
        }
        triggerAsyncUpdate();
        return;
    }

    if (type == WorkerMessage::openEditor)
    {
        const auto index = stream.readInt();
        MessageManager::callAsync ([this, index]
        {
            auto reportResult = [this, index] (bool opened, const String& error)
            {
                sendMessageToCoordinator (makeMessage (WorkerMessage::editorStatus,
                    [index, opened, &error] (MemoryOutputStream& output)
                    {
                        output.writeInt (index);
                        output.writeBool (opened);
                        output.writeString (error);
                    }));
            };

            if (chain == nullptr || index < 0 || index >= chain->size())
            {
                reportResult (false, "The helper could not find that plug-in in the rack.");
                return;
            }

            auto node = (*chain)[index].node;
            if (node == nullptr || node->getProcessor() == nullptr)
            {
                reportResult (false, "The plug-in did not load in the helper, so it has no editor to open.");
                return;
            }

            for (auto* component : editorWindows)
                if (auto* existing = dynamic_cast<WorkerEditorWindow*> (component))
                    if (existing->getSlotIndex() == index)
                {
                    existing->setVisible (true);
                    existing->toFront (true);
                    reportResult (existing->isShowing(), existing->isShowing() ? String()
                        : "Windows did not show the plug-in editor window.");
                    return;
                }

            if (auto* editor = node->getProcessor()->createEditorIfNeeded())
            {
                auto* window = new WorkerEditorWindow (std::move (node), editor, index,
                    getSlotIdentity ((*chain)[index]));
                editorWindows.push_back (window);
                reportResult (window->isShowing(), window->isShowing() ? String()
                    : "The plug-in editor was created, but Windows did not show its window.");
            }
            else
                reportResult (false, "This plug-in does not provide an editor window.");
        });
        return;
    }

    if (type == WorkerMessage::closeEditors)
    {
        MessageManager::callAsync ([this]
        {
            for (auto* window : editorWindows)
                delete window;
            editorWindows.clear();
        });
        return;
    }

    if (type == WorkerMessage::shutdown)
        JUCEApplicationBase::quit();
}

void PluginRackWorkerServer::handleConnectionLost()
{
    shuttingDown.store (true, std::memory_order_release);
    signalThreadShouldExit();
    if (shared != nullptr)
        shared->workerAlive.store (0, std::memory_order_release);
    MessageManager::callAsync ([] { JUCEApplicationBase::quit(); });
}

void PluginRackWorkerServer::handleAsyncUpdate()
{
    String mapPath, rackXml;
    bool doInitialise = false;
    bool doRack = false;
    bool doReplace = false;
    double sampleRate = 48000.0;
    int blockSize = 512;
    uint32_t generation = 0;
    {
        const ScopedLock lock (commandLock);
        doInitialise = initialisePending;
        initialisePending = false;
        doRack = rackPending;
        rackPending = false;
        doReplace = replacePending;
        replacePending = false;
        mapPath = pendingSharedPath;
        rackXml = pendingRackXml;
        generation = pendingRackGeneration;
        sampleRate = pendingSampleRate;
        blockSize = pendingBlockSize;
    }

    if (doInitialise)
    {
        setupMapping (mapPath, sampleRate, blockSize);
        if (shared != nullptr)
        {
            shared->workerAlive.store (1, std::memory_order_release);
            // The main device callback is realtime; keep the isolated DSP
            // thread schedulable enough to finish each shared block before
            // the host needs its delayed result.
            startThread (Thread::Priority::high);
            sendMessageToCoordinator (makeMessage (WorkerMessage::workerReady));
        }
        else
        {
            sendMessageToCoordinator (makeMessage (WorkerMessage::workerError, [] (MemoryOutputStream& stream)
            {
                stream.writeString ("The plug-in worker could not open its shared audio memory.");
            }));
        }
    }

    if (shared != nullptr)
    {
        const auto newRate = jlimit (8000.0, 768000.0, sampleRate);
        const auto newBlock = jlimit (16, maxSamples, blockSize);
        shared->sampleRate.store ((uint32_t) newRate, std::memory_order_release);
        shared->blockSize.store ((uint32_t) newBlock, std::memory_order_release);

        if (doReplace)
            applyRack (rackXml, true, generation);
        else if (doRack)
            applyRack (rackXml, false, generation);
        else if (! doInitialise
                 && (graph.getSampleRate() != newRate || graph.getBlockSize() != newBlock))
        {
            graphPaused.store (true, std::memory_order_release);
            const auto waitStart = Time::getMillisecondCounter();
            while (processingAudio.load (std::memory_order_acquire)
                   && (uint32_t) (Time::getMillisecondCounter() - waitStart) < 3000)
                Thread::sleep (1);

            if (processingAudio.load (std::memory_order_acquire))
            {
                graphPaused.store (false, std::memory_order_release);
                sendMessageToCoordinator (makeMessage (WorkerMessage::workerError, [] (MemoryOutputStream& stream)
                {
                    stream.writeString ("The audio format changed while a plug-in was still processing.");
                }));
                return;
            }

            graph.releaseResources();
            graph.setRateAndBufferSizeDetails (newRate, newBlock);
            graph.prepareToPlay (newRate, newBlock);
            graphPaused.store (false, std::memory_order_release);
        }
    }
}

void PluginRackWorkerServer::timerCallback()
{
    if (chain == nullptr || ! isThreadRunning())
        return;

    sendMessageToCoordinator (makeMessage (WorkerMessage::workerHeartbeat));

    const auto now = Time::getMillisecondCounter();
    for (auto& watcher : stateWatchers)
    {
        if (! watcher->dirty.load (std::memory_order_acquire))
            continue;

        const auto changedAt = watcher->changedAt.load (std::memory_order_relaxed);
        if ((uint32_t) (now - changedAt) < 800)
            continue;

        watcher->dirty.store (false, std::memory_order_release);
        if (watcher->changedAt.load (std::memory_order_relaxed) != changedAt)
        {
            watcher->dirty.store (true, std::memory_order_release);
            continue;
        }

        const auto index = watcher->index;
        if (index < 0 || index >= chain->size() || (*chain)[index].node == nullptr)
            continue;

        MemoryBlock state;
        (*chain)[index].node->getProcessor()->getStateInformation (state);
        const auto generation = activeRackGeneration;
        const auto message = makeMessage (WorkerMessage::parameterState, [index, generation, &state] (MemoryOutputStream& stream)
        {
            stream.writeInt64 ((int64) generation);
            stream.writeInt (index);
            stream.writeInt ((int) state.getSize());
            stream.write (state.getData(), state.getSize());
        });
        sendMessageToCoordinator (message);
    }
}

void PluginRackWorkerServer::setupMapping (const String& path, double sampleRate, int blockSize)
{
    sharedFile = File (path);
    sharedMapping = std::make_unique<MemoryMappedFile> (sharedFile, MemoryMappedFile::readWrite);
    if (sharedMapping->getData() == nullptr || sharedMapping->getSize() < mappingSize())
        return;

    shared = static_cast<SharedAudio*> (sharedMapping->getData());
    if (shared->sharedMagic != magic || shared->sharedVersion != version)
    {
        shared = nullptr;
        sharedMapping.reset();
        return;
    }

    const auto rate = jlimit (8000.0, 768000.0, sampleRate);
    const auto block = jlimit (16, maxSamples, blockSize);
    graph.setRateAndBufferSizeDetails (rate, block);
    graph.prepareToPlay (rate, block);
}

void PluginRackWorkerServer::applyRack (const String& xmlText, bool forceReplace, uint32_t generation)
{
    const auto parsed = XmlDocument::parse (xmlText);
    if (parsed == nullptr || ! parsed->hasTagName ("pluginchain"))
    {
        sendMessageToCoordinator (makeMessage (WorkerMessage::workerError, [] (MemoryOutputStream& stream)
        {
            stream.writeString ("The host sent an invalid rack description.");
        }));
        return;
    }

    graphPaused.store (true, std::memory_order_release);
    const auto waitStart = Time::getMillisecondCounter();
    while (processingAudio.load (std::memory_order_acquire)
           && (uint32_t) (Time::getMillisecondCounter() - waitStart) < 3000)
        Thread::sleep (1);

    if (processingAudio.load (std::memory_order_acquire))
    {
        graphPaused.store (false, std::memory_order_release);
        sendMessageToCoordinator (makeMessage (WorkerMessage::workerError, [] (MemoryOutputStream& stream)
        {
            stream.writeString ("A plug-in is still processing. The helper kept the rack intact.");
        }));
        return;
    }

    if (forceReplace)
    {
        for (auto* window : editorWindows)
            delete window;
        editorWindows.clear();
    }

    stateWatchers.clear();
    graph.releaseResources();
    if (forceReplace)
    {
        chain->loadFromPresetXml (parsed.get());
        for (int index = 0; index < chain->size(); ++index)
            (*chain)[index].errorMessage.clear();
        chain->prepareEmptyGraph();
        chain->loadAll();
    }
    else
    {
        chain->reconcileFromPresetXml (parsed.get());

        for (int windowIndex = (int) editorWindows.size(); --windowIndex >= 0;)
        {
            auto* window = dynamic_cast<WorkerEditorWindow*> (editorWindows[(size_t) windowIndex]);
            if (window == nullptr)
                continue;

            int matchingIndex = -1;
            for (int slotIndex = 0; slotIndex < chain->size(); ++slotIndex)
                if (getSlotIdentity ((*chain)[slotIndex]) == window->getInstanceIdentity())
                {
                    matchingIndex = slotIndex;
                    break;
                }

            if (matchingIndex < 0)
            {
                delete window;
                editorWindows.erase (editorWindows.begin() + windowIndex);
            }
            else
            {
                window->setSlotIndex (matchingIndex);
            }
        }
    }
    graph.prepareToPlay ((double) shared->sampleRate.load(), (int) shared->blockSize.load());
    for (int index = 0; index < chain->size(); ++index)
        if ((*chain)[index].node != nullptr)
            stateWatchers.push_back (std::make_unique<StateWatcher> (
                *(*chain)[index].node->getProcessor(), index));
    graphPaused.store (false, std::memory_order_release);
    activeRackGeneration = generation;
    shared->activeRackGeneration.store (generation, std::memory_order_release);
    reportRackStatus();
}

void PluginRackWorkerServer::reportRackStatus()
{
    int loadedCount = 0;
    int failedCount = 0;
    if (chain != nullptr)
        for (int index = 0; index < chain->size(); ++index)
            ((*chain)[index].node != nullptr && ! (*chain)[index].isFailed()
                ? loadedCount : failedCount)++;
    if (shared != nullptr)
    {
        shared->loadedPlugins.store ((uint32_t) loadedCount, std::memory_order_release);
        shared->failedPlugins.store ((uint32_t) failedCount, std::memory_order_release);
    }

    const auto message = makeMessage (WorkerMessage::rackStatus, [this] (MemoryOutputStream& stream)
    {
        stream.writeInt64 ((int64) activeRackGeneration);
        stream.writeInt (chain != nullptr ? chain->size() : 0);
        if (chain == nullptr)
            return;

        for (int index = 0; index < chain->size(); ++index)
        {
            const auto& slot = (*chain)[index];
            const bool loaded = slot.node != nullptr && ! slot.isFailed();
            stream.writeInt (index);
            stream.writeBool (loaded);
            stream.writeString (loaded ? String() : (slot.errorMessage.isNotEmpty()
                ? slot.errorMessage : "Plug-in could not be loaded in the worker."));
        }
    });
    sendMessageToCoordinator (message);
}

void PluginRackWorkerServer::run()
{
    AudioBuffer<float> buffer (maxChannels, maxSamples);
    MidiBuffer midi;

    while (! threadShouldExit())
    {
        bool processed = false;
        if (shared != nullptr && ! graphPaused.load (std::memory_order_acquire)
            && shared->workerAlive.load (std::memory_order_acquire) != 0)
        {
            AudioSlot* next = nullptr;
            for (auto& slot : shared->slots)
                if (slot.state.load (std::memory_order_acquire) == 1
                    && (next == nullptr || (int32_t) (slot.sequence - next->sequence) < 0))
                    next = &slot;

            if (next != nullptr)
            {
                int expected = 1;
                if (next->state.compare_exchange_strong (expected, 4, std::memory_order_acq_rel))
                {
                auto& slot = *next;

                const auto generation = slot.rackGeneration;
                if (generation != shared->activeRackGeneration.load (std::memory_order_acquire)
                    || generation != shared->requestedRackGeneration.load (std::memory_order_acquire))
                {
                    slot.state.store (0, std::memory_order_release);
                    continue;
                }

                if (slot.deadlineMissed.load (std::memory_order_acquire))
                {
                    slot.state.store (0, std::memory_order_release);
                    continue;
                }

                const auto samples = jlimit (1, maxSamples, slot.samples);
                // The scratch buffer is allocated at maxSamples, but the graph
                // must see this slot's actual size. Processing the full scratch
                // buffer made small device callbacks do up to 32x unnecessary
                // work and routinely miss the output deadline.
                buffer.setSize (maxChannels, samples, false, false, true);
                buffer.clear (0, samples);
                buffer.clear (1, samples);
                FloatVectorOperations::copy (buffer.getWritePointer (0), slot.input[0], samples);
                FloatVectorOperations::copy (buffer.getWritePointer (1), slot.input[1], samples);
                midi.clear();
                appendMidi (midi, slot.midi, jlimit (0, maxMidiBytes, slot.midiBytes));

                processingAudio.store (true, std::memory_order_release);
                if (graphPaused.load (std::memory_order_acquire))
                {
                    processingAudio.store (false, std::memory_order_release);
                    slot.state.store (0, std::memory_order_release);
                    continue;
                }
                graph.processBlock (buffer, midi);
                processingAudio.store (false, std::memory_order_release);

                FloatVectorOperations::copy (slot.output[0], buffer.getReadPointer (0), samples);
                FloatVectorOperations::copy (slot.output[1], buffer.getReadPointer (1), samples);
                uint32_t changedFrames = 0;
                for (int sample = 0; sample < samples; ++sample)
                    if (std::abs (slot.output[0][sample] - slot.input[0][sample]) > 1.0e-6f
                        || std::abs (slot.output[1][sample] - slot.input[1][sample]) > 1.0e-6f)
                        ++changedFrames;
                shared->changedFrames.fetch_add (changedFrames, std::memory_order_relaxed);
                writeMidi (midi, slot.midiOutput, maxMidiBytes, slot.midiOutputBytes);
                if (! slot.deadlineMissed.load (std::memory_order_acquire)
                    && generation == shared->activeRackGeneration.load (std::memory_order_acquire)
                    && generation == shared->requestedRackGeneration.load (std::memory_order_acquire))
                {
                    slot.state.store (2, std::memory_order_release);
                    shared->completedBlocks.fetch_add (1, std::memory_order_release);
                }
                else
                    slot.state.store (0, std::memory_order_release);
                processed = true;
                }
            }
        }

        if (! processed)
            wait (1);
    }
}
