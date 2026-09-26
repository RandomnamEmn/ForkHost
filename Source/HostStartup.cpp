#include <juce_audio_processors/juce_audio_processors.h>
#include "IconMenu.hpp"
#include "HostOptions.hpp"
#include "IsolatedPluginScanner.hpp"
#include "AudioDeviceInitHelpers.hpp"
#include <iostream>

#if ! (JUCE_PLUGINHOST_VST || JUCE_PLUGINHOST_VST3 || JUCE_PLUGINHOST_AU)
 #error "If you're building the audio plugin host, you probably want to enable VST and/or AU support"
#endif

class PluginHostApp  : public JUCEApplication
{
public:
    PluginHostApp() = default;

    void initialise (const String&) override
    {
        const auto commandLine = getCommandLineParameterArray();
        if (AudioDeviceInitHelpers::isMidiProbeHelperCommandLine (commandLine))
        {
            setApplicationReturnValue (AudioDeviceInitHelpers::runMidiProbeHelper());
            quit();
            return;
        }

        if (isPluginScanHelperCommandLine (commandLine))
        {
            setApplicationReturnValue (runPluginScanHelper (commandLine));
            quit();
            return;
        }

        auto parsed = parseCommandLine();

        if (parsed.showHelp)
        {
            std::cout << usageText().toStdString() << std::endl;
            setApplicationReturnValue (0);
            quit();
            return;
        }

        if (parsed.showVersion)
        {
            std::cout << getApplicationName().toStdString() << " "
                      << getApplicationVersion().toStdString() << std::endl;
            setApplicationReturnValue (0);
            quit();
            return;
        }

        if (parsed.error.isNotEmpty())
        {
            std::cerr << "Light Host: " << parsed.error.toStdString() << "\n\n"
                      << usageText().toStdString() << std::endl;
            setApplicationReturnValue (2);
            quit();
            return;
        }

        PropertiesFile::Options options;
        options.applicationName     = getApplicationName();
        options.filenameSuffix      = "settings";
        options.osxLibrarySubFolder = "Preferences";

        checkArguments (&options, parsed.options);

        appProperties = std::make_unique<ApplicationProperties>();
        appProperties->setStorageParameters (options);

        LookAndFeel::setDefaultLookAndFeel (&lookAndFeel);

        mainWindow = std::make_unique<IconMenu> (parsed.options);
        mainWindow->openMainWindow();
        #if JUCE_MAC || JUCE_LINUX
            Process::setDockIconVisible(false);
        #endif
    }

    void shutdown() override
    {
        mainWindow = nullptr;
        appProperties = nullptr;
        LookAndFeel::setDefaultLookAndFeel (nullptr);
    }

    void systemRequestedQuit() override
    {
        JUCEApplicationBase::quit();
    }

    const String getApplicationName() override       { return "Light Host"; }
    const String getApplicationVersion() override    { return "1.2.0"; }

    bool moreThanOneInstanceAllowed() override
    {
        if (hasFlag ("--internal-plugin-scan-helper"))
            return true;

        if (hasFlag (AudioDeviceInitHelpers::midiProbeHelperArgument))
            return true;

        // Debug sessions are intentionally isolated and are commonly launched
        // while a normal Light Host instance is already running.
        if (hasFlag ("--debug"))
            return true;

        StringArray multiInstance = getParameter ("-multi-instance");
        return multiInstance.size() == 2;
    }

    ApplicationCommandManager commandManager;
    std::unique_ptr<ApplicationProperties> appProperties;
    LookAndFeel_V4 lookAndFeel;

private:
    struct ParsedCommandLine
    {
        HostOptions options;
        String error;
        bool showHelp = false;
        bool showVersion = false;
    };

    std::unique_ptr<IconMenu> mainWindow;

    static String usageText()
    {
        return
            "Usage: Light Host CLI [options]\n"
            "\n"
            "Options:\n"
            "  --debug                     Use debugger-safe offline mode. The real plugin GUI\n"
            "                              remains interactive, but no real-time audio thread runs.\n"
            "  --plugin <path>             Load a plugin directly from a VST3/AU path. Repeatable.\n"
            "  --plugin-name <name>        Select a type from the most recent --plugin when a shell\n"
            "                              contains multiple plugin types (for example WaveShell).\n"
            "  --append                    Append CLI plugins to the persisted chain instead of\n"
            "                              starting with an isolated chain.\n"
            "  --no-editor                 Do not automatically open loaded plugin editors.\n"
            "  --sample-rate <hz>          Debug-device sample rate (default: 48000).\n"
            "  --block-size <samples>      Debug-device block size (default: 512).\n"
            "  --process-blocks <count>    Process this many silent blocks once at startup.\n"
            "  --exit-after-process        Exit after --process-blocks completes.\n"
            "  --help, -h                  Show this help.\n"
            "  --version                   Show the version.\n"
            "\n"
            "Legacy option:\n"
            "  -multi-instance=<suffix>    Use a separate settings suffix and allow another instance.";
    }

    ParsedCommandLine parseCommandLine() const
    {
        ParsedCommandLine parsed;
        const auto args = getCommandLineParameterArray();

        auto isRecognizedOptionToken = [] (const String& token)
        {
            static const StringArray flagOptions {
                "--debug", "--append", "--no-editor", "--exit-after-process",
                "--help", "-h", "--version",
                "--plugin", "--plugin-name", "--sample-rate", "--block-size",
                "--process-blocks"
            };

            if (flagOptions.contains (token) || token.startsWith ("-multi-instance="))
                return true;

            static const StringArray valueOptions {
                "--plugin=", "--plugin-name=", "--sample-rate=",
                "--block-size=", "--process-blocks="
            };

            for (const auto& prefix : valueOptions)
                if (token.startsWith (prefix))
                    return true;

            return false;
        };

        auto takeValue = [&] (int& index, const String& option, String& value) -> bool
        {
            const auto& arg = args[index];
            const String prefix = option + "=";

            if (arg.startsWith (prefix))
            {
                value = arg.substring (prefix.length());
                return value.isNotEmpty();
            }

            if (arg == option && index + 1 < args.size())
            {
                const auto& candidate = args[index + 1];
                if (candidate.startsWith ("--") || isRecognizedOptionToken (candidate))
                    return false;

                value = candidate;
                ++index;
                return value.isNotEmpty();
            }

            return false;
        };

        for (int i = 0; i < args.size(); ++i)
        {
            const String arg = args[i];

            if (arg == "--help" || arg == "-h")
            {
                parsed.showHelp = true;
                continue;
            }

            if (arg == "--version")
            {
                parsed.showVersion = true;
                continue;
            }

            if (arg == "--debug")
            {
                parsed.options.debugMode = true;
                continue;
            }

            if (arg == "--append")
            {
                parsed.options.appendPlugins = true;
                continue;
            }

            if (arg == "--no-editor")
            {
                parsed.options.openEditors = false;
                continue;
            }

            if (arg == "--exit-after-process")
            {
                parsed.options.exitAfterProcess = true;
                continue;
            }

            // Keep the original multi-instance syntax supported by the GUI app.
            if (arg.startsWith ("-multi-instance="))
                continue;

            String value;

            if (arg == "--plugin" || arg.startsWith ("--plugin="))
            {
                if (!takeValue (i, "--plugin", value))
                {
                    parsed.error = "--plugin requires a path.";
                    return parsed;
                }

                parsed.options.plugins.push_back ({ value, {} });
                continue;
            }

            if (arg == "--plugin-name" || arg.startsWith ("--plugin-name="))
            {
                if (!takeValue (i, "--plugin-name", value))
                {
                    parsed.error = "--plugin-name requires a name.";
                    return parsed;
                }

                if (parsed.options.plugins.empty())
                {
                    parsed.error = "--plugin-name must immediately follow a --plugin option.";
                    return parsed;
                }

                parsed.options.plugins.back().name = value;
                continue;
            }

            if (arg == "--sample-rate" || arg.startsWith ("--sample-rate="))
            {
                if (!takeValue (i, "--sample-rate", value))
                {
                    parsed.error = "--sample-rate requires a positive number.";
                    return parsed;
                }

                const double sampleRate = value.getDoubleValue();
                if (sampleRate <= 0.0)
                {
                    parsed.error = "Invalid --sample-rate value: " + value;
                    return parsed;
                }

                parsed.options.sampleRate = sampleRate;
                continue;
            }

            if (arg == "--block-size" || arg.startsWith ("--block-size="))
            {
                if (!takeValue (i, "--block-size", value))
                {
                    parsed.error = "--block-size requires a positive integer.";
                    return parsed;
                }

                const int blockSize = value.getIntValue();
                if (blockSize <= 0)
                {
                    parsed.error = "Invalid --block-size value: " + value;
                    return parsed;
                }

                parsed.options.blockSize = blockSize;
                continue;
            }

            if (arg == "--process-blocks" || arg.startsWith ("--process-blocks="))
            {
                if (!takeValue (i, "--process-blocks", value))
                {
                    parsed.error = "--process-blocks requires a positive integer.";
                    return parsed;
                }

                const int blockCount = value.getIntValue();
                if (blockCount <= 0)
                {
                    parsed.error = "Invalid --process-blocks value: " + value;
                    return parsed;
                }

                parsed.options.processBlocks = blockCount;
                continue;
            }

            parsed.error = "Unknown option: " + arg;
            return parsed;
        }

        if ((parsed.options.processBlocks > 0 || parsed.options.exitAfterProcess)
            && !parsed.options.debugMode)
        {
            parsed.error = "--process-blocks and --exit-after-process require --debug.";
        }
        else if (parsed.options.exitAfterProcess && parsed.options.processBlocks <= 0)
        {
            parsed.error = "--exit-after-process requires --process-blocks.";
        }

        return parsed;
    }

    bool hasFlag (const String& flag) const
    {
        return getCommandLineParameterArray().contains (flag);
    }

    StringArray getParameter (const String& lookFor) const
    {
        const String prefix = lookFor + "=";
        StringArray parameters = getCommandLineParameterArray();
        StringArray found;

        for (int i = 0; i < parameters.size(); ++i)
        {
            String param = parameters[i];
            if (param.startsWith(prefix))
            {
                String val = param.substring(prefix.length());
                if (val.isNotEmpty())
                {
                    found.add(lookFor);
                    found.add(val);
                    return found;
                }
            }
        }

        return found;
    }

    void checkArguments (PropertiesFile::Options* options, const HostOptions& hostOptions)
    {
        String suffix = hostOptions.debugMode ? "debug.settings" : "settings";

        StringArray multiInstance = getParameter ("-multi-instance");
        if (multiInstance.size() == 2)
            suffix = multiInstance[1] + "." + suffix;

        options->filenameSuffix = suffix;
    }
};

static PluginHostApp& getApp()                      { return *dynamic_cast<PluginHostApp*>(JUCEApplication::getInstance()); }
ApplicationCommandManager& getCommandManager()      { return getApp().commandManager; }
ApplicationProperties& getAppProperties()           { return *getApp().appProperties; }

START_JUCE_APPLICATION (PluginHostApp)
