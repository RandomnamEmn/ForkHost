#ifndef HostOptions_hpp
#define HostOptions_hpp

#include <juce_core/juce_core.h>
#include <vector>

struct PluginLaunchRequest
{
    juce::String path;
    juce::String name;
};

struct HostOptions
{
    bool debugMode = false;
    bool openEditors = true;
    bool appendPlugins = false;
    bool exitAfterProcess = false;
    bool useSeparateHelper = false;

    double sampleRate = 48000.0;
    int blockSize = 512;
    int processBlocks = 0;

    std::vector<PluginLaunchRequest> plugins;
};

#endif // HostOptions_hpp
