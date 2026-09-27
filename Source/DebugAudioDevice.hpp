#ifndef DebugAudioDevice_hpp
#define DebugAudioDevice_hpp

#include <juce_audio_devices/juce_audio_devices.h>

using namespace juce;

//==============================================================================
/**
    A synthetic stereo device used by ForkHost's debugger-friendly mode.

    The device reports ordinary stereo input/output channels and calls
    audioDeviceAboutToStart(), so AudioProcessorPlayer prepares the graph with
    the requested sample rate and block size. It intentionally never calls the
    real-time audio callback. This leaves the JUCE message loop and hosted
    plugin editors fully interactive while making it safe to stop the process
    at a debugger breakpoint for an arbitrary amount of time.

    Audio is advanced explicitly by IconMenu::processDebugBlocks().
*/
class DebugAudioIODevice final : public AudioIODevice
{
public:
    static String deviceName() { return "Debug I/O"; }
    static String typeName()   { return "Debug"; }

    DebugAudioIODevice() : AudioIODevice (deviceName(), typeName()) {}

    StringArray getOutputChannelNames() override { return { "Output 1", "Output 2" }; }
    StringArray getInputChannelNames() override  { return { "Input 1", "Input 2" }; }

    BigInteger getActiveOutputChannels() const override { return outputChannels; }
    BigInteger getActiveInputChannels() const override  { return inputChannels; }

    Array<double> getAvailableSampleRates() override
    {
        return { 22050.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    }

    Array<int> getAvailableBufferSizes() override
    {
        return { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };
    }

    int getDefaultBufferSize() override        { return 512; }
    int getCurrentBufferSizeSamples() override { return bufferSize; }
    double getCurrentSampleRate() override     { return sampleRate; }
    int getCurrentBitDepth() override          { return 32; }

    int getOutputLatencyInSamples() override   { return 0; }
    int getInputLatencyInSamples() override    { return 0; }
    int getXRunCount() const noexcept override { return 0; }

    String open (const BigInteger& inputs,
                 const BigInteger& outputs,
                 double requestedSampleRate,
                 int requestedBufferSize) override
    {
        inputChannels = inputs;
        outputChannels = outputs;
        sampleRate = requestedSampleRate > 0.0 ? requestedSampleRate : 48000.0;
        bufferSize = requestedBufferSize > 0 ? requestedBufferSize : 512;
        open_ = true;
        return {};
    }

    void close() override
    {
        stop();
        open_ = false;
    }

    void start (AudioIODeviceCallback* newCallback) override
    {
        callback = newCallback;

        if (callback != nullptr && open_)
        {
            callback->audioDeviceAboutToStart (this);
            playing_ = true;
        }

        // Deliberately do not create a worker thread or invoke
        // audioDeviceIOCallbackWithContext().
    }

    void stop() override
    {
        if (playing_ && callback != nullptr)
            callback->audioDeviceStopped();

        playing_ = false;
        callback = nullptr;
    }

    bool isOpen() override    { return open_; }
    bool isPlaying() override { return playing_; }

    String getLastError() override { return {}; }

private:
    AudioIODeviceCallback* callback = nullptr;
    BigInteger inputChannels;
    BigInteger outputChannels;
    double sampleRate = 48000.0;
    int bufferSize = 512;
    bool open_ = false;
    bool playing_ = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DebugAudioIODevice)
};

//==============================================================================
class DebugAudioIODeviceType final : public AudioIODeviceType
{
public:
    DebugAudioIODeviceType() : AudioIODeviceType (DebugAudioIODevice::typeName()) {}

    void scanForDevices() override { scanned = true; }

    StringArray getDeviceNames (bool) const override
    {
        jassert (scanned);
        return { DebugAudioIODevice::deviceName() };
    }

    int getDefaultDeviceIndex (bool) const override
    {
        jassert (scanned);
        return 0;
    }

    int getIndexOfDevice (AudioIODevice* device, bool) const override
    {
        jassert (scanned);
        return dynamic_cast<DebugAudioIODevice*> (device) != nullptr ? 0 : -1;
    }

    bool hasSeparateInputsAndOutputs() const override { return false; }

    AudioIODevice* createDevice (const String&, const String&) override
    {
        jassert (scanned);
        return new DebugAudioIODevice();
    }

private:
    bool scanned = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DebugAudioIODeviceType)
};

#endif // DebugAudioDevice_hpp
