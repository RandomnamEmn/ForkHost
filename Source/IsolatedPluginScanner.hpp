#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <memory>

class PluginScanLog
{
public:
    explicit PluginScanLog (juce::File reportFile);

    void beginScan (const juce::StringArray& previouslyBlacklisted);
    void recordFailure (const juce::String& format,
                        const juce::String& fileOrIdentifier,
                        const juce::String& reason,
                        const juce::String& threadStacks = {});
    void finishScan();
    bool clearResults();

    int getEntryCount() const;
    juce::File getReportFile() const;

private:
    void appendLine (const juce::String& line);

    juce::CriticalSection lock;
    juce::File reportFile;
    juce::File stackReportFile;
    int entryCount = 0;
};

class IsolatedPluginScanner final : public juce::KnownPluginList::CustomScanner
{
public:
    explicit IsolatedPluginScanner (std::shared_ptr<PluginScanLog> scanLog);

    bool findPluginTypesFor (juce::AudioPluginFormat& format,
                             juce::OwnedArray<juce::PluginDescription>& result,
                             const juce::String& fileOrIdentifier) override;

private:
    std::shared_ptr<PluginScanLog> scanLog;
};

bool isPluginScanHelperCommandLine (const juce::StringArray& arguments);
int runPluginScanHelper (const juce::StringArray& arguments);
