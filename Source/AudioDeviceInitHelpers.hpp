#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <chrono>

namespace AudioDeviceInitHelpers
{
inline constexpr auto midiProbeHelperArgument = "--internal-midi-probe-helper";

// Run the JUCE MIDI query in a disposable process. A timed-out worker thread
// cannot be cancelled and may leave JUCE's MIDI singleton and hidden device
// change window tied to a stuck thread in the host process.
inline bool probeMidiService (std::chrono::milliseconds timeout)
{
    juce::StringArray command;
    command.add (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName());
    command.add (midiProbeHelperArgument);

    juce::ChildProcess probe;
    if (! probe.start (command, 0))
        return false;

    if (! probe.waitForProcessToFinish ((int) timeout.count()))
    {
        probe.kill();
        probe.waitForProcessToFinish (2000);
        return false;
    }

    return probe.getExitCode() == 0;
}

inline bool isMidiProbeHelperCommandLine (const juce::StringArray& arguments)
{
    return arguments.size() == 1 && arguments[0] == midiProbeHelperArgument;
}

inline int runMidiProbeHelper()
{
    try
    {
        (void) juce::MidiInput::getAvailableDevices();
        (void) juce::MidiOutput::getAvailableDevices();
        return 0;
    }
    catch (...)
    {
        return 1;
    }
}

// Initialise only the audio side of AudioDeviceManager. Passing saved XML to
// JUCE also enumerates MIDI devices synchronously, so rebuild the audio setup
// and use the overload that does not restore MIDI state when MIDI is offline.
inline juce::String initialiseAudioWithoutMidi (juce::AudioDeviceManager& manager,
                                                 int numInputChannels,
                                                 int numOutputChannels,
                                                 const juce::XmlElement* savedState)
{
    if (savedState == nullptr)
        return manager.initialise (numInputChannels, numOutputChannels, nullptr, false);

    const auto savedType = savedState->getStringAttribute ("deviceType");
    if (savedType.isNotEmpty() && savedType != manager.getCurrentAudioDeviceType())
        manager.setCurrentAudioDeviceType (savedType, false);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    const auto sharedDeviceName = savedState->getStringAttribute ("audioDeviceName");

    if (sharedDeviceName.isNotEmpty())
    {
        setup.inputDeviceName = sharedDeviceName;
        setup.outputDeviceName = sharedDeviceName;
    }
    else
    {
        setup.inputDeviceName = savedState->getStringAttribute ("audioInputDeviceName");
        setup.outputDeviceName = savedState->getStringAttribute ("audioOutputDeviceName");
    }

    setup.bufferSize = savedState->getIntAttribute ("audioDeviceBufferSize", setup.bufferSize);
    setup.sampleRate = savedState->getDoubleAttribute ("audioDeviceRate", setup.sampleRate);
    setup.inputChannels.parseString (savedState->getStringAttribute ("audioDeviceInChans", "11"), 2);
    setup.outputChannels.parseString (savedState->getStringAttribute ("audioDeviceOutChans", "11"), 2);
    setup.useDefaultInputChannels = ! savedState->hasAttribute ("audioDeviceInChans");
    setup.useDefaultOutputChannels = ! savedState->hasAttribute ("audioDeviceOutChans");

    return manager.initialise (numInputChannels, numOutputChannels, nullptr, false, {}, &setup);
}

// When MIDI is skipped for this run, keep the saved MIDI selection in settings
// so a later launch can restore it after the Windows service responds.
inline void preserveMidiSettings (const juce::XmlElement* savedState, juce::XmlElement& currentState)
{
    if (savedState == nullptr)
        return;

    for (const auto* attribute : { "defaultMidiOutput", "defaultMidiOutputDevice" })
        if (savedState->hasAttribute (attribute))
            currentState.setAttribute (attribute, savedState->getStringAttribute (attribute));

    for (int i = currentState.getNumChildElements() - 1; i >= 0; --i)
        if (currentState.getChildElement (i)->hasTagName ("MIDIINPUT"))
            currentState.removeChildElement (currentState.getChildElement (i), true);

    for (auto* child = savedState->getFirstChildElement(); child != nullptr; child = child->getNextElement())
        if (child->hasTagName ("MIDIINPUT"))
            currentState.addChildElement (new juce::XmlElement (*child));
}
}
