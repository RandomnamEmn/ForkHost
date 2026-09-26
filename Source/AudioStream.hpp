//
//  AudioStream.hpp
//  Light Host
//
//  Audio processor player with gain-ramp (fade) and MIDI input support.
//  Extracted from IconMenu as a reusable audio infrastructure class.
//

#ifndef AudioStream_hpp
#define AudioStream_hpp

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <cmath>

using namespace juce;

//==============================================================================
/**
    An AudioProcessorPlayer that applies a gain ramp (fade-in / fade-out)
    over the output buffers and forwards enabled MIDI inputs into the graph.

    PluginChain owns the actual MIDI connections between graph nodes so MIDI
    follows the same user-visible plug-in order as the chain.

    Call fadeTo(target, rampMs) to start a linear ramp from the current gain
    to the target over the specified duration.  The ramp is sample-accurate
    and runs on the audio thread with no extra allocations.

    Use setGainImmediately(gain) to snap the gain without a ramp.
*/
class AudioStream  : public AudioProcessorPlayer,
                     private ChangeListener
{
public:
    explicit AudioStream (AudioDeviceManager& dm)
        : AudioProcessorPlayer (false), midiDeviceManager (dm), transportPlayHead (*this)
    {
        // Listen to every MIDI input enabled in AudioDeviceManager.  Passing an
        // empty device identifier means newly enabled devices start forwarding
        // immediately without needing to re-register the callback.
        midiDeviceManager.addMidiInputDeviceCallback ({}, this);
    }

    ~AudioStream() override
    {
        if (currentGraph != nullptr)
        {
            currentGraph->removeChangeListener (this);
            if (currentGraph->getPlayHead() == &transportPlayHead)
                currentGraph->setPlayHead (nullptr);
        }

        midiDeviceManager.removeMidiInputDeviceCallback ({}, this);
    }

    static constexpr int fadeRampMs   = 5;
    static constexpr int fadeExtraMs  = 5;

    /** When true, gain transitions use a linear ramp (fadeIn/fadeOut).
        When false, gain changes are instant (snap).  The ramp duration
        is always the fadeRampMs / fadeExtraMs compile-time constants —
        unchecking simply skips the ramp, re-checking restores it. */
    std::atomic<bool> fadeEnabled{ true };

    /** A positive default BPM reports a continuously advancing, playing host
        transport to hosted plug-ins. Zero or a negative value reports stopped. */
    void setDefaultBpm (double bpm) noexcept
    {
        if (! std::isfinite (bpm))
            bpm = 0.0;

        defaultBpm.store (jlimit (-100000.0, 100000.0, bpm));
    }

    double getDefaultBpm() const noexcept { return defaultBpm.load(); }

    /** Called when the audio device reports a runtime error
        (e.g. after sleep/wake or device disconnection). */
    std::function<void(const String&)> onDeviceError;

    /** Called when the audio device has been stopped. */
    std::function<void()> onDeviceStopped;

    //==============================================================================
    /**
        Attach a processor and, when it is an AudioProcessorGraph, keep the
        graph's MIDI input node available.  PluginChain is responsible for
        connecting that node through hosted plug-ins in chain order.

        AudioProcessorPlayer::setProcessor is not virtual, so this intentionally
        hides it for AudioStream call sites.
    */
    void setProcessor (AudioProcessor* processorToSet)
    {
        if (currentGraph != nullptr)
        {
            currentGraph->removeChangeListener (this);
            if (currentGraph->getPlayHead() == &transportPlayHead)
                currentGraph->setPlayHead (nullptr);
        }

        AudioProcessorPlayer::setProcessor (processorToSet);
        currentGraph = dynamic_cast<AudioProcessorGraph*> (processorToSet);

        if (currentGraph != nullptr)
        {
            currentGraph->setPlayHead (&transportPlayHead);
            currentGraph->addChangeListener (this);
            ensureMidiInputNode();
        }
    }

    //==============================================================================
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const AudioIODeviceCallbackContext& context) override
    {
        transportPlayHead.setBlockPosition (transportSamples.load(), transportPpq.load());
        AudioProcessorPlayer::audioDeviceIOCallbackWithContext (inputChannelData,
                                                                 numInputChannels,
                                                                 outputChannelData,
                                                                 numOutputChannels,
                                                                 numSamples,
                                                                 context);
        const auto bpm = defaultBpm.load();
        if (bpm > 0.0)
        {
            const auto rate = sampleRate.load();
            transportSamples.fetch_add (numSamples);
            if (rate > 0.0)
                transportPpq.store (transportPpq.load() + (double) numSamples * bpm / (rate * 60.0));
        }

        int rem = remainingSamples.load();
        if (rem > 0)
        {
            const int total = totalRampSamples.load();
            const float target = targetGain.load();
            const float start = startGain.load();
            for (int s = 0; s < numSamples; ++s)
            {
                float gain;
                if (rem > 0)
                {
                    const float progress = 1.0f - (float) rem / (float) total;
                    gain = start + (target - start) * progress;
                    curGain = gain;
                    --rem;
                }
                else { gain = target; }
                for (int c = 0; c < numOutputChannels; ++c)
                    outputChannelData[c][s] *= gain;
            }
            remainingSamples.store (rem);
        }
        else
        {
            const float gain = targetGain.load();
            if (gain != 1.0f)
                for (int c = 0; c < numOutputChannels; ++c)
                    FloatVectorOperations::multiply (outputChannelData[c], gain, numSamples);
        }
    }

    //==============================================================================
    void audioDeviceAboutToStart (AudioIODevice* device) override
    {
        AudioProcessorPlayer::audioDeviceAboutToStart (device);
        sampleRate = device->getCurrentSampleRate();
        // Fade in to eliminate pop when switching devices (e.g., Loopback ↔ WASAPI)
        setGainImmediately (0.0f);
        if (fadeEnabled)
            fadeTo (1.0f, fadeRampMs);
        else
            setGainImmediately (1.0f);
    }

    //==============================================================================
    void audioDeviceError (const String& message) override
    {
        AudioProcessorPlayer::audioDeviceError (message);
        if (onDeviceError)
            onDeviceError (message);
    }

    //==============================================================================
    void audioDeviceStopped() override
    {
        AudioProcessorPlayer::audioDeviceStopped();
        if (onDeviceStopped)
            onDeviceStopped();
    }

    //==============================================================================
    /** Start a linear gain ramp from the current gain to @a target
        over @a rampMs milliseconds.
    */
    void fadeTo (float target, int rampMs)
    {
        startGain.store (curGain);
        const int ramp = jmax (1, (int) (sampleRate * rampMs / 1000));
        totalRampSamples.store (ramp);

        if (target <= curGain)
        {
            targetGain.store (target);
            remainingSamples.store (ramp);
        }
        else
        {
            remainingSamples.store (ramp);
            targetGain.store (target);
        }
    }

    //==============================================================================
    void suspend (AudioDeviceManager& dm)
    {
        dm.removeAudioCallback (this);
        setProcessor (nullptr);
    }

    void resume (AudioDeviceManager& dm, AudioProcessorGraph& graph)
    {
        setProcessor (&graph);
        dm.addAudioCallback (this);
        setGainImmediately (0.0f);
        if (fadeEnabled)
            fadeTo (1.0f, fadeRampMs);
        else
            setGainImmediately (1.0f);
    }

    /** Attach the graph with output muted while an old graph sequence drains. */
    void resumeMuted (AudioDeviceManager& dm, AudioProcessorGraph& graph)
    {
        setProcessor (&graph);
        setGainImmediately (0.0f);
        dm.addAudioCallback (this);
    }

    //==============================================================================
    /** Set the gain immediately (no ramp). */
    void setGainImmediately (float gain)
    {
        curGain = gain;
        startGain.store (gain);
        targetGain.store (gain);
        remainingSamples.store (0);
        totalRampSamples.store (0);
    }

private:
    class TransportPlayHead final : public AudioPlayHead
    {
    public:
        explicit TransportPlayHead (const AudioStream& streamIn) : stream (streamIn) {}

        void setBlockPosition (int64_t samples, double ppq) noexcept
        {
            blockSamples.store (samples);
            blockPpq.store (ppq);
        }

    private:
        Optional<PositionInfo> getPosition() const override
        {
            PositionInfo position;
            const auto samples = blockSamples.load();
            const auto rate = stream.sampleRate.load();
            const auto bpm = stream.defaultBpm.load();
            const auto seconds = rate > 0.0 ? (double) samples / rate : 0.0;

            position.setTimeInSamples (samples);
            position.setTimeInSeconds (seconds);
            position.setPpqPosition (blockPpq.load());
            position.setTimeSignature (TimeSignature { 4, 4 });
            position.setIsPlaying (bpm > 0.0);
            if (bpm > 0.0)
                position.setBpm (bpm);

            return position;
        }

        const AudioStream& stream;
        std::atomic<int64_t> blockSamples { 0 };
        std::atomic<double> blockPpq { 0.0 };
    };

    //==============================================================================
    void changeListenerCallback (ChangeBroadcaster* source) override
    {
        if (source == currentGraph)
            ensureMidiInputNode();
    }

    void ensureMidiInputNode()
    {
        if (currentGraph == nullptr || updatingMidiRouting)
            return;

        const ScopedValueSetter<bool> routingGuard (updatingMidiRouting, true);
        const AudioProcessorGraph::NodeID midiInputId (1000002);

        if (currentGraph->getNodeForId (midiInputId) == nullptr)
        {
            currentGraph->addNode (
                std::make_unique<AudioProcessorGraph::AudioGraphIOProcessor> (
                    AudioProcessorGraph::AudioGraphIOProcessor::midiInputNode),
                midiInputId);
        }
    }

    AudioDeviceManager& midiDeviceManager;
    AudioProcessorGraph* currentGraph = nullptr;
    bool updatingMidiRouting = false;

    std::atomic<double> sampleRate{ 44100.0 };
    std::atomic<double> defaultBpm{ 0.0 };
    std::atomic<int64_t> transportSamples{ 0 };
    std::atomic<double> transportPpq{ 0.0 };
    TransportPlayHead transportPlayHead;
    std::atomic<float> curGain{ 1.0f };
    std::atomic<float> startGain{ 1.0f };
    std::atomic<float> targetGain{ 1.0f };
    std::atomic<int> remainingSamples{ 0 };
    std::atomic<int> totalRampSamples{ 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioStream)
};

#endif // AudioStream_hpp
