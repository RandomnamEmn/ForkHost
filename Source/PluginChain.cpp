//
//  PluginChain.cpp
//  Light Host
//
//  Unified plugin effect chain management.
//  Replaces the old activePluginList + getTimeSortedList() approach.
//

#include "PluginChain.hpp"
#include "PluginWindow.h"
#include <juce_audio_processors/juce_audio_processors.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <limits>

namespace
{
constexpr int maximumLatencyDrainMs = 1000;

bool isSamePluginType (const PluginDescription& a, const PluginDescription& b)
{
    if (! a.pluginFormatName.equalsIgnoreCase (b.pluginFormatName)
        || a.uniqueId != b.uniqueId)
        return false;

    if (a.fileOrIdentifier.isNotEmpty() || b.fileOrIdentifier.isNotEmpty())
        return a.fileOrIdentifier.equalsIgnoreCase (b.fileOrIdentifier);

    return a.name.equalsIgnoreCase (b.name)
        && a.manufacturerName.equalsIgnoreCase (b.manufacturerName);
}

#if JUCE_DEBUG
void writePluginLoadTrace (const String& message)
{
    static CriticalSection traceLock;
    const ScopedLock lock (traceLock);
    auto file = File::getSpecialLocation (File::tempDirectory)
                    .getChildFile ("LightHostReforge-plugin-load.log");
    FileOutputStream stream (file);

    if (stream.openedOk())
    {
        stream.setPosition (file.getSize());
        stream.writeText (Time::getCurrentTime().toISO8601 (true) + " " + message + "\n",
                          false, false, nullptr);
        stream.flush();
    }
}
#else
void writePluginLoadTrace (const String&) {}
#endif
}

//==============================================================================
PluginChain::PluginChain (AudioProcessorGraph& graphRef,
                          AudioPluginFormatManager& fmRef,
                          AudioStream& audioStreamRef,
                          bool nonRealtimeMode)
    : graph (graphRef), formatManager (fmRef), audioStream (audioStreamRef),
      nonRealtime (nonRealtimeMode)
{
}

PluginChain::~PluginChain()
{
}

//==============================================================================
// Chain operations
//==============================================================================

int PluginChain::add (const PluginDescription& desc)
{
    fadeOut();

    PluginSlot slot;
    slot.desc = desc;
    slot.bypassed = false;
    ensureInstanceSequence (slot);

    String errorMessage;
    auto instance = formatManager.createPluginInstance (desc,
        graph.getSampleRate(), graph.getBlockSize(), errorMessage);

    if (instance == nullptr)
    {
        slot.errorMessage = errorMessage;
        slot.node = nullptr;
    }
    else
    {
        instance->setNonRealtime (nonRealtime);
        instance->setRateAndBufferSizeDetails (graph.getSampleRate(), graph.getBlockSize());

        // Capture default state for persistence
        instance->getStateInformation (slot.state);

        slot.node = graph.addNode (std::move (instance), std::nullopt);
    }

    chain.push_back (std::move (slot));
    connectChain();
    fadeIn();
    return (int) chain.size() - 1;
}

void PluginChain::addSlot (PluginSlot&& slot)
{
    ensureInstanceSequence (slot);
    chain.push_back (std::move (slot));
}

int64_t PluginChain::allocateInstanceSequence()
{
    if (nextInstanceSequence <= 0
        || nextInstanceSequence == std::numeric_limits<int64_t>::max())
    {
        std::vector<PluginSlot*> orderedSlots;
        orderedSlots.reserve (chain.size());
        for (auto& slot : chain)
            orderedSlots.push_back (&slot);

        std::stable_sort (orderedSlots.begin(), orderedSlots.end(),
            [] (const PluginSlot* a, const PluginSlot* b)
            {
                return a->instanceSequence < b->instanceSequence;
            });

        int64_t sequence = 1;
        for (auto* slot : orderedSlots)
            slot->instanceSequence = sequence++;

        nextInstanceSequence = sequence;
    }

    while (std::any_of (chain.begin(), chain.end(), [this] (const PluginSlot& slot)
                        { return slot.instanceSequence == nextInstanceSequence; }))
        ++nextInstanceSequence;

    return nextInstanceSequence++;
}

void PluginChain::ensureInstanceSequence (PluginSlot& slot)
{
    const auto sequenceIsUsed = std::any_of (chain.begin(), chain.end(), [&slot] (const PluginSlot& current)
    {
        return current.instanceSequence == slot.instanceSequence;
    });

    if (slot.instanceSequence <= 0
        || slot.instanceSequence == std::numeric_limits<int64_t>::max()
        || sequenceIsUsed)
    {
        slot.instanceSequence = allocateInstanceSequence();
        return;
    }

    if (slot.instanceSequence >= nextInstanceSequence)
        nextInstanceSequence = slot.instanceSequence + 1;
}

String PluginChain::getDisplayName (int index) const
{
    if (index < 0 || index >= (int) chain.size())
        return {};

    const auto& slot = chain[(size_t) index];
    int duplicateNumber = 1;
    for (const auto& other : chain)
        if (isSamePluginType (slot.desc, other.desc)
            && other.instanceSequence < slot.instanceSequence)
            ++duplicateNumber;

    String pluginName = slot.desc.name;
    if (duplicateNumber > 1)
        pluginName << " (" << duplicateNumber << ")";

    return slot.instanceLabel.isNotEmpty()
        ? slot.instanceLabel + " — " + pluginName
        : pluginName;
}

bool PluginChain::setInstanceLabel (int index, const String& label)
{
    if (index < 0 || index >= (int) chain.size())
        return false;

    auto cleanedLabel = label.trim().replaceCharacters ("\r\n\t", "   ").trim();
    if (cleanedLabel.length() > 80)
        cleanedLabel = cleanedLabel.substring (0, 80);

    chain[(size_t) index].instanceLabel = cleanedLabel;
    return true;
}

bool PluginChain::reload (int index)
{
    if (index < 0 || index >= (int) chain.size())
        return false;

    auto& slot = chain[(size_t) index];
    auto replacementNodeId = slot.node != nullptr
        ? slot.node->nodeID
        : AudioProcessorGraph::NodeID {};

    if (slot.node != nullptr)
    {
        slot.node->getProcessor()->getStateInformation (slot.state);
        PluginWindow::closeCurrentlyOpenWindowsFor (slot.node->nodeID);
        graph.removeNode (slot.node->nodeID);
        slot.node = nullptr;
    }

    slot.errorMessage.clear();
    String errorMessage;
    auto instance = formatManager.createPluginInstance (slot.desc,
        graph.getSampleRate(), graph.getBlockSize(), errorMessage);

    if (instance == nullptr)
    {
        slot.errorMessage = errorMessage.isNotEmpty()
            ? errorMessage : "Plug-in instance creation failed";
        connectChain();
        return false;
    }

    // A slot's graph node ID is unrelated to its current rack position. Keep
    // the existing ID after a reorder. Failed slots have no node to retain, so
    // give them a currently unused ID instead of assuming index + 1 is free.
    if (replacementNodeId.uid == 0)
    {
        uint32_t candidate = 1;
        constexpr uint32_t firstReservedHostNodeId = 1000000;
        while (candidate < firstReservedHostNodeId
               && graph.getNodeForId (AudioProcessorGraph::NodeID (candidate)) != nullptr)
            ++candidate;

        if (candidate == firstReservedHostNodeId)
        {
            slot.errorMessage = "No free audio graph node ID is available";
            connectChain();
            return false;
        }

        replacementNodeId = AudioProcessorGraph::NodeID (candidate);
    }

    instance->setNonRealtime (nonRealtime);
    instance->setRateAndBufferSizeDetails (graph.getSampleRate(), graph.getBlockSize());
    if (slot.hasSavedState())
        instance->setStateInformation (slot.state.getData(), (int) slot.state.getSize());

    slot.node = graph.addNode (std::move (instance), replacementNodeId);
    if (slot.node == nullptr)
        slot.errorMessage = "Plug-in was created but could not be added to the audio graph";

    connectChain();
    return slot.node != nullptr;
}

bool PluginChain::remove (int index)
{
    if (index < 0 || index >= (int) chain.size())
        return false;

    auto& slot = chain[(size_t) index];

    // Close the plugin's window before removing the node.
    if (slot.node != nullptr)
    {
        PluginWindow::closeCurrentlyOpenWindowsFor (slot.node->nodeID);
        graph.removeNode (slot.node->nodeID);
    }

    chain.erase (chain.begin() + index);
    connectChain();
    return true;
}

bool PluginChain::moveUp (int index)
{
    if (index <= 0 || index >= (int) chain.size())
        return false;

    fadeOut();
    std::swap (chain[(size_t) index], chain[(size_t) (index - 1)]);
    connectChain();
    fadeIn();
    return true;
}

bool PluginChain::moveDown (int index)
{
    if (index < 0 || index >= (int) chain.size() - 1)
        return false;

    fadeOut();
    std::swap (chain[(size_t) index], chain[(size_t) (index + 1)]);
    connectChain();
    fadeIn();
    return true;
}

void PluginChain::toggleBypass (int index)
{
    if (index < 0 || index >= (int) chain.size())
        return;

    fadeOut();

    auto& slot = chain[(size_t) index];
    slot.bypassed = !slot.bypassed;

    connectChain();
    fadeIn();
}

void PluginChain::clear()
{
    // Step 1: Close all plugin editor windows.  For standard (non-VST3)
    // editors this destroys the ToolbarComponent and its child editor
    // component, which nulls the SafePointer in ToolbarComponent and
    // calls editorBeingDeleted() on the AudioProcessor.  VST3 editors
    // (VST3PluginWindow in JUCE 8) may survive this step because the
    // VST3 plugin's IPlugView retains a COM reference that prevents
    // complete teardown — those are handled in Step 2.
    PluginWindow::closeAllCurrentlyOpenWindows();

    // Step 2: Explicitly delete any editors that survived window
    // closure.  The ToolbarComponent's SafePointer<Component> member
    // auto-nulls when the editor component is destroyed, so the
    // PluginWindow destructor's listener-removal path is safe even
    // when the editor has already been freed.
    for (auto& node : graph.getNodes())
        if (auto* editor = node->getProcessor()->getActiveEditor())
            delete editor;

    chain.clear();
}

bool PluginChain::moveTo (int fromIndex, int toIndex)
{
    if (fromIndex < 0 || fromIndex >= (int) chain.size()
        || toIndex < 0 || toIndex >= (int) chain.size()
        || fromIndex == toIndex)
        return false;

    fadeOut();
    auto slot = std::move (chain[(size_t) fromIndex]);
    chain.erase (chain.begin() + fromIndex);
    chain.insert (chain.begin() + toIndex, std::move (slot));
    connectChain();
    fadeIn();
    return true;
}

//==============================================================================

void PluginChain::fadeOut()
{
    if (audioStream.fadeEnabled)
    {
        audioStream.fadeTo (0.0f, AudioStream::fadeRampMs);
        // Let the audio callback finish the gain ramp without dispatching a
        // second rack action in the middle of this graph mutation.
        Thread::sleep (AudioStream::fadeRampMs + 10);
    }
    else
    {
        audioStream.setGainImmediately (0.0f);
    }

    int totalLatencySamples = getTotalPluginLatencySamples();
    if (totalLatencySamples > 0)
    {
        double sr = graph.getSampleRate();
        if (sr > 0)
        {
            const auto requestedDrainMs = (double) totalLatencySamples / sr * 1000.0;
            const auto boundedDrainMs = std::isfinite (requestedDrainMs)
                ? jlimit (0.0, (double) maximumLatencyDrainMs, requestedDrainMs)
                : (double) maximumLatencyDrainMs;
            int drainMs = (int) boundedDrainMs;
            if (audioStream.fadeEnabled)
                drainMs = jmin (maximumLatencyDrainMs, drainMs + AudioStream::fadeExtraMs);
            Thread::sleep (drainMs);
        }
        else if (audioStream.fadeEnabled)
        {
            Thread::sleep (AudioStream::fadeExtraMs);
        }
    }
}

void PluginChain::fadeIn()
{
    audioStream.setGainImmediately (0.0f);
    if (audioStream.fadeEnabled)
        audioStream.fadeTo (1.0f, AudioStream::fadeRampMs);
    else
        audioStream.setGainImmediately (1.0f);
}

int PluginChain::getTotalPluginLatencySamples() const
{
    int total = 0;
    for (const auto& slot : chain)
        if (slot.node != nullptr && !slot.bypassed && !slot.isFailed())
        {
            const auto latency = jmax (0, slot.node->getProcessor()->getLatencySamples());
            if (latency > INT_MAX - total)
                return INT_MAX;

            total += latency;
        }
    return total;
}

//==============================================================================
// Graph management
//==============================================================================

void PluginChain::loadAll()
{
    writePluginLoadTrace ("loadAll begin, slots=" + String ((int) chain.size()));
    prepareEmptyGraph();

    // Build plugin nodes
    for (int i = 0; i < (int) chain.size(); i++)
    {
        auto& slot = chain[(size_t) i];
        writePluginLoadTrace ("slot " + String (i) + " " + slot.desc.name
                              + " create begin, savedStateBytes="
                              + String ((int) slot.state.getSize()));

        // Skip previously failed plugins
        if (slot.isFailed())
        {
            slot.node = nullptr;
            continue;
        }

        String errorMessage;
        auto instance = formatManager.createPluginInstance (slot.desc,
            graph.getSampleRate(), graph.getBlockSize(), errorMessage);

        writePluginLoadTrace ("slot " + String (i) + " " + slot.desc.name
                              + " create returned, success=" + (instance != nullptr ? "yes" : "no")
                              + (errorMessage.isNotEmpty() ? ", error=" + errorMessage : String()));

        if (instance == nullptr)
        {
            slot.errorMessage = errorMessage;
            slot.node = nullptr;
            continue;
        }

        instance->setNonRealtime (nonRealtime);
        instance->setRateAndBufferSizeDetails (graph.getSampleRate(), graph.getBlockSize());

        // Restore saved state if available
        if (slot.hasSavedState())
        {
            writePluginLoadTrace ("slot " + String (i) + " " + slot.desc.name
                                  + " setStateInformation begin");
            instance->setStateInformation (slot.state.getData(), (int) slot.state.getSize());
            writePluginLoadTrace ("slot " + String (i) + " " + slot.desc.name
                                  + " setStateInformation returned");
        }
        // Note: slot.state intentionally NOT re-captured here.  The state
        // decoded from the preset XML is retained as-is; the next persistence
        // operation captures current processor state before serialising.

        writePluginLoadTrace ("slot " + String (i) + " " + slot.desc.name + " addNode begin");
        slot.node = graph.addNode (std::move (instance),
            AudioProcessorGraph::NodeID (i + 1));
        writePluginLoadTrace ("slot " + String (i) + " " + slot.desc.name + " addNode returned");
    }

    // Reconnect
    writePluginLoadTrace ("connectChain begin");
    connectChain();
    writePluginLoadTrace ("loadAll complete");
}

void PluginChain::prepareEmptyGraph()
{
    // Assumes audio is paused. Installing this empty topology lets the audio
    // thread adopt it and retire the old render sequence before a preset
    // creates another instance of the same plug-in.
    graph.clear();

    const AudioProcessorGraph::NodeID INPUT (1000000);
    const AudioProcessorGraph::NodeID OUTPUT (INPUT.uid + 1);

    graph.addNode (std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor>(
        AudioProcessorGraph::AudioGraphIOProcessor::audioInputNode), INPUT);
    graph.addNode (std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor>(
        AudioProcessorGraph::AudioGraphIOProcessor::audioOutputNode), OUTPUT);
    connectChain();
}

//==============================================================================
// Internal connection logic
//==============================================================================

void PluginChain::connectChain()
{
    const AudioProcessorGraph::NodeID INPUT (1000000);
    const AudioProcessorGraph::NodeID OUTPUT (INPUT.uid + 1);
    const AudioProcessorGraph::NodeID MIDI_INPUT (INPUT.uid + 2);

    // The audio player injects hardware MIDI into the graph's MIDI input node.
    // Keep the node stable and let this class own all MIDI connections so they
    // follow the exact user-visible plug-in order.
    if (graph.getNodeForId (MIDI_INPUT) == nullptr)
    {
        graph.addNode (
            std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor> (
                AudioProcessorGraph::AudioGraphIOProcessor::midiInputNode),
            MIDI_INPUT);
    }

    // Remove all existing audio and MIDI connections before rebuilding both
    // paths from the ordered chain.
    auto connections = graph.getConnections();
    for (auto& c : connections)
        graph.removeConnection (c);

    auto conn = [&](AudioProcessorGraph::NodeID src, int sc,
                     AudioProcessorGraph::NodeID dst, int dc)
    {
        const AudioProcessorGraph::Connection connection { { src, sc }, { dst, dc } };
        if (graph.canConnect (connection))
            graph.addConnection (connection);
    };

    auto connectAudio = [&](AudioProcessorGraph::NodeID src, int srcChannels,
                            AudioProcessorGraph::NodeID dst, int dstChannels)
    {
        srcChannels = jlimit (0, 2, srcChannels);
        dstChannels = jlimit (0, 2, dstChannels);

        if (srcChannels == 0 || dstChannels == 0)
            return;

        const int sharedChannels = jmin (srcChannels, dstChannels);
        for (int channel = 0; channel < sharedChannels; ++channel)
            conn (src, channel, dst, channel);

        // Preserve a mono source when feeding a stereo destination rather than
        // leaving the right-hand channel disconnected.
        if (srcChannels == 1 && dstChannels == 2)
            conn (src, 0, dst, 1);
    };

    auto midiConn = [&](AudioProcessorGraph::NodeID src,
                         AudioProcessorGraph::NodeID dst)
    {
        const AudioProcessorGraph::Connection connection {
            { src, AudioProcessorGraph::midiChannelIndex },
            { dst, AudioProcessorGraph::midiChannelIndex }
        };

        if (graph.canConnect (connection))
            graph.addConnection (connection);
    };

    // Start with the host's stereo audio input.  MIDI-only processors leave
    // this source untouched; instruments (no audio inputs, audio outputs)
    // replace it; audio sinks can tap it without breaking downstream audio.
    AudioProcessorGraph::NodeID currentAudioSource = INPUT;
    int currentAudioSourceChannels = 2;

    // MIDI starts at the hardware-input node.  A processor that accepts MIDI
    // receives the current stream.  Only processors that actually produce MIDI
    // become the source for subsequent plug-ins; synths such as Serum normally
    // consume MIDI without producing it, so they do not cut the stream off.
    AudioProcessorGraph::NodeID currentMidiSource = MIDI_INPUT;
    bool hasMidiSource = graph.getNodeForId (MIDI_INPUT) != nullptr;

    for (int i = 0; i < (int) chain.size(); i++)
    {
        const auto& slot = chain[(size_t) i];

        // Failed or bypassed plugins are skipped in both paths.
        if (slot.isFailed() || slot.bypassed || slot.node == nullptr)
            continue;

        auto nodeId = slot.node->nodeID;
        auto* processor = slot.node->getProcessor();
        if (processor == nullptr)
            continue;

        const int audioInputs = processor->getTotalNumInputChannels();
        const int audioOutputs = processor->getTotalNumOutputChannels();

        if (audioInputs > 0)
            connectAudio (currentAudioSource, currentAudioSourceChannels,
                          nodeId, audioInputs);

        // A processor with audio outputs becomes the new serial audio source.
        // This covers ordinary effects and generators/instruments.  Processors
        // with no audio outputs (for example MIDI-only processors or analyzers)
        // do not sever the existing audio path.
        if (audioOutputs > 0)
        {
            currentAudioSource = nodeId;
            currentAudioSourceChannels = audioOutputs;
        }

        // MIDI follows the user-visible chain order.  MIDI processors therefore
        // modify the stream seen by later processors/instruments instead of every
        // node receiving the original keyboard events in parallel.
        if (hasMidiSource && processor->acceptsMidi())
            midiConn (currentMidiSource, nodeId);

        if (processor->producesMidi())
        {
            currentMidiSource = nodeId;
            hasMidiSource = true;
        }
    }

    // Always finish the current audio source at the host output.  With no active
    // audio processors this is simply the stereo input -> output passthrough.
    connectAudio (currentAudioSource, currentAudioSourceChannels, OUTPUT, 2);
}

//==============================================================================
// Queries
//==============================================================================

int PluginChain::getSlotIndexForNode (AudioProcessorGraph::NodeID nodeId) const
{
    for (int i = 0; i < (int) chain.size(); i++)
    {
        const auto& slot = chain[(size_t) i];
        if (slot.node != nullptr && slot.node->nodeID == nodeId)
            return i;
    }
    return -1;
}

int PluginChain::getChainPositionForNode (AudioProcessorGraph::NodeID nodeId) const
{
    // The chain position is the same as the slot index
    // (the vector IS the ordered chain)
    return getSlotIndexForNode (nodeId);
}

//==============================================================================
// Persistence
//==============================================================================

void PluginChain::loadFromProperties (ApplicationProperties& props)
{
    auto xml = props.getUserSettings()->getXmlValue ("pluginChain");
    if (xml != nullptr)
    {
        loadFromPresetXml (xml.get());
    }
}

void PluginChain::saveToProperties (ApplicationProperties& props)
{
    // App-property persistence is also used on shutdown, so refresh the cached
    // state from every live processor first.  Otherwise parameter edits made
    // since the last explicit preset save would be lost after a restart.
    for (auto& slot : chain)
    {
        if (slot.node != nullptr && !slot.isFailed())
            slot.node->getProcessor()->getStateInformation (slot.state);
    }

    auto xml = createPresetXml();
    if (xml != nullptr)
    {
        props.getUserSettings()->setValue ("pluginChain", xml.get());
        props.saveIfNeeded();
    }
}

std::unique_ptr<XmlElement> PluginChain::createPresetXml() const
{
    auto root = std::make_unique<XmlElement> ("pluginchain");

    for (int i = 0; i < (int) chain.size(); i++)
    {
        const auto& slot = chain[(size_t) i];

        auto pluginXml = std::make_unique<XmlElement> ("plugin");
        pluginXml->setAttribute ("bypassed", slot.bypassed);
        pluginXml->setAttribute ("instanceLabel", slot.instanceLabel);
        pluginXml->setAttribute ("instanceSequence", String (slot.instanceSequence));

        if (slot.errorMessage.isNotEmpty())
            pluginXml->setAttribute ("error", slot.errorMessage);

        if (auto descXml = slot.desc.createXml())
            pluginXml->addChildElement (descXml.release());

        if (slot.hasSavedState())
        {
            auto stateXml = std::make_unique<XmlElement> ("state");
            stateXml->addTextElement (slot.state.toBase64Encoding());
            pluginXml->addChildElement (stateXml.release());
        }

        root->addChildElement (pluginXml.release());
    }

    return root;
}

void PluginChain::loadFromPresetXml (const XmlElement* xml)
{
    jassert (xml != nullptr && xml->hasTagName ("pluginchain"));
    if (xml == nullptr || !xml->hasTagName ("pluginchain"))
        return;

    clear();

    for (auto* pluginXml = xml->getFirstChildElement();
         pluginXml != nullptr;
         pluginXml = pluginXml->getNextElement())
    {
        if (!pluginXml->hasTagName ("plugin"))
            continue;

        PluginSlot slot;
        slot.bypassed     = pluginXml->getBoolAttribute ("bypassed", false);
        slot.errorMessage = pluginXml->getStringAttribute ("error", "");
        slot.instanceLabel = pluginXml->getStringAttribute ("instanceLabel");
        slot.instanceSequence = pluginXml->getStringAttribute ("instanceSequence").getLargeIntValue();

        if (auto* descXml = pluginXml->getFirstChildElement())
        {
            if (descXml->hasTagName ("PLUGIN"))
                slot.desc.loadFromXml (*descXml);
        }

        if (slot.desc.name.isEmpty() && pluginXml->hasAttribute ("name"))
        {
            slot.desc.name             = pluginXml->getStringAttribute ("name");
            slot.desc.pluginFormatName = pluginXml->getStringAttribute ("format");
            slot.desc.version          = pluginXml->getStringAttribute ("version");
            slot.desc.fileOrIdentifier = pluginXml->getStringAttribute ("file", slot.desc.fileOrIdentifier);
            slot.desc.uniqueId         = (int) pluginXml->getIntAttribute ("uid", slot.desc.uniqueId);
        }

        if (auto* stateXml = pluginXml->getChildByName ("state"))
        {
            String stateBase64 = stateXml->getAllSubText().trim();
            if (stateBase64.isNotEmpty())
                slot.state.fromBase64Encoding(stateBase64);
        }

        ensureInstanceSequence (slot);
        chain.push_back (std::move (slot));
    }
}
