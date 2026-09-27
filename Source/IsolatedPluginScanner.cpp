#include "IsolatedPluginScanner.hpp"

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <dbghelp.h>
 #include <tlhelp32.h>
 #include <string>
#endif

namespace
{
constexpr auto helperArgument = "--internal-plugin-scan-helper";
constexpr int scanTimeoutMs = 30000;

#if JUCE_WINDOWS
std::wstring quoteWindowsArgument (const juce::String& argument)
{
    const std::wstring input (argument.toWideCharPointer());
    std::wstring output (L"\"");
    size_t backslashes = 0;

    for (const auto character : input)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }

        if (character == L'\"')
        {
            output.append (backslashes * 2 + 1, L'\\');
            output += L'\"';
            backslashes = 0;
            continue;
        }

        output.append (backslashes, L'\\');
        backslashes = 0;
        output += character;
    }

    output.append (backslashes * 2, L'\\');
    output += L'\"';
    return output;
}

class ScannerHelperProcess
{
public:
    ~ScannerHelperProcess()
    {
        if (info.hThread != nullptr) CloseHandle (info.hThread);
        if (info.hProcess != nullptr) CloseHandle (info.hProcess);
        if (job != nullptr) CloseHandle (job);
    }

    bool start (const juce::StringArray& arguments)
    {
        if (arguments.isEmpty())
            return false;

        job = CreateJobObjectW (nullptr, nullptr);
        if (job == nullptr)
            return false;

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits {};
        jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (! SetInformationJobObject (job, JobObjectExtendedLimitInformation,
                                       &jobLimits, sizeof (jobLimits)))
            return false;

        auto application = arguments[0].toWideCharPointer();
        std::wstring commandLine;
        for (const auto& argument : arguments)
        {
            if (! commandLine.empty()) commandLine += L' ';
            commandLine += quoteWindowsArgument (argument);
        }

        STARTUPINFOW startup {};
        startup.cb = sizeof (startup);
        if (! CreateProcessW (application, commandLine.data(), nullptr, nullptr, FALSE,
                              CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED,
                              nullptr, nullptr, &startup, &info))
            return false;

        // Assign before the helper runs so any processes started by a plug-in
        // during scanning are contained in the same kill-on-close job.
        if (! AssignProcessToJobObject (job, info.hProcess))
        {
            TerminateProcess (info.hProcess, 1);
            WaitForSingleObject (info.hProcess, 2000);
            return false;
        }

        if (ResumeThread (info.hThread) == (DWORD) -1)
        {
            TerminateJobObject (job, 1);
            WaitForSingleObject (info.hProcess, 2000);
            return false;
        }

        return true;
    }

    bool isRunning() const
    {
        return info.hProcess != nullptr && WaitForSingleObject (info.hProcess, 0) != WAIT_OBJECT_0;
    }

    bool waitForProcessToFinish (int timeoutMs) const
    {
        return info.hProcess != nullptr
            && WaitForSingleObject (info.hProcess, (DWORD) juce::jmax (0, timeoutMs)) == WAIT_OBJECT_0;
    }

    juce::uint32 getExitCode() const
    {
        DWORD code = 0;
        if (info.hProcess != nullptr) GetExitCodeProcess (info.hProcess, &code);
        return code;
    }

    bool kill() const
    {
        if (info.hProcess == nullptr)
            return false;
        return job != nullptr ? TerminateJobObject (job, 0) != FALSE
                              : TerminateProcess (info.hProcess, 0) != FALSE;
    }

    juce::String getThreadStacks() const
    {
        if (info.hProcess == nullptr || ! isRunning())
            return "No live helper process was available for stack capture.";

        const auto processId = info.dwProcessId;
        SymSetOptions (SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
        if (! SymInitializeW (info.hProcess, nullptr, TRUE))
            return "Symbol initialization failed with Windows error "
                 + juce::String ((int) GetLastError()) + ".";

        juce::String stacks;
        stacks << "Helper process " << juce::String ((int) processId) << juce::newLine;

        HANDLE snapshot = CreateToolhelp32Snapshot (TH32CS_SNAPTHREAD, 0);
        if (snapshot == INVALID_HANDLE_VALUE)
        {
            stacks << "Thread enumeration failed with Windows error "
                   << juce::String ((int) GetLastError()) << "." << juce::newLine;
            SymCleanup (info.hProcess);
            return stacks;
        }

        THREADENTRY32 entry {};
        entry.dwSize = sizeof (entry);
        if (Thread32First (snapshot, &entry))
        {
            do
            {
                if (entry.th32OwnerProcessID != processId)
                    continue;

                stacks << "\nThread " << juce::String ((int) entry.th32ThreadID) << ":" << juce::newLine;
                HANDLE thread = OpenThread (THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT
                                            | THREAD_QUERY_INFORMATION,
                                            FALSE, entry.th32ThreadID);
                if (thread == nullptr)
                {
                    stacks << "  OpenThread failed with Windows error "
                           << juce::String ((int) GetLastError()) << "." << juce::newLine;
                    continue;
                }

                const auto previousSuspendCount = SuspendThread (thread);
                if (previousSuspendCount == (DWORD) -1)
                {
                    stacks << "  SuspendThread failed with Windows error "
                           << juce::String ((int) GetLastError()) << "." << juce::newLine;
                    CloseHandle (thread);
                    continue;
                }

                CONTEXT context {};
                context.ContextFlags = CONTEXT_FULL;
                if (! GetThreadContext (thread, &context))
                {
                    stacks << "  GetThreadContext failed with Windows error "
                           << juce::String ((int) GetLastError()) << "." << juce::newLine;
                    ResumeThread (thread);
                    CloseHandle (thread);
                    continue;
                }

                STACKFRAME64 frame {};
               #if defined (_M_X64) || defined (__x86_64__)
                constexpr DWORD machineType = IMAGE_FILE_MACHINE_AMD64;
                frame.AddrPC.Offset = context.Rip;
                frame.AddrFrame.Offset = context.Rbp;
                frame.AddrStack.Offset = context.Rsp;
               #elif defined (_M_IX86) || defined (__i386__)
                constexpr DWORD machineType = IMAGE_FILE_MACHINE_I386;
                frame.AddrPC.Offset = context.Eip;
                frame.AddrFrame.Offset = context.Ebp;
                frame.AddrStack.Offset = context.Esp;
               #else
                constexpr DWORD machineType = 0;
               #endif
                frame.AddrPC.Mode = AddrModeFlat;
                frame.AddrFrame.Mode = AddrModeFlat;
                frame.AddrStack.Mode = AddrModeFlat;

               #if defined (_M_X64) || defined (__x86_64__) || defined (_M_IX86) || defined (__i386__)
                for (int frameIndex = 0; frameIndex < 80; ++frameIndex)
                {
                    const auto address = frame.AddrPC.Offset;
                    if (address == 0 || ! StackWalk64 (machineType, info.hProcess, thread, &frame,
                                                        &context, nullptr, SymFunctionTableAccess64,
                                                        SymGetModuleBase64, nullptr))
                        break;

                    char symbolStorage[sizeof (SYMBOL_INFO) + MAX_SYM_NAME] {};
                    auto* symbol = reinterpret_cast<SYMBOL_INFO*> (symbolStorage);
                    symbol->SizeOfStruct = sizeof (SYMBOL_INFO);
                    symbol->MaxNameLen = MAX_SYM_NAME;
                    DWORD64 displacement = 0;

                    if (SymFromAddr (info.hProcess, address, &displacement, symbol))
                    {
                        stacks << "  " << juce::String (frameIndex).paddedLeft (' ', 2) << "  "
                               << juce::String::fromUTF8 (symbol->Name) << "+0x"
                               << juce::String::toHexString ((juce::int64) displacement)
                               << " [0x" << juce::String::toHexString ((juce::int64) address)
                               << "]" << juce::newLine;
                    }
                    else
                    {
                        IMAGEHLP_MODULE64 module {};
                        module.SizeOfStruct = sizeof (module);
                        if (SymGetModuleInfo64 (info.hProcess, address, &module))
                            stacks << "  " << juce::String (frameIndex).paddedLeft (' ', 2) << "  "
                                   << juce::String (module.ModuleName) << "+0x"
                                   << juce::String::toHexString ((juce::int64) (address - module.BaseOfImage))
                                   << " [0x" << juce::String::toHexString ((juce::int64) address)
                                   << "]" << juce::newLine;
                        else
                            stacks << "  " << juce::String (frameIndex).paddedLeft (' ', 2)
                                   << "  0x" << juce::String::toHexString ((juce::int64) address)
                                   << juce::newLine;
                    }
                }
               #else
                stacks << "  Stack walking is unsupported for this Windows architecture."
                       << juce::newLine;
               #endif

                ResumeThread (thread);
                CloseHandle (thread);
            }
            while (Thread32Next (snapshot, &entry));
        }

        CloseHandle (snapshot);
        SymCleanup (info.hProcess);
        return stacks;
    }

private:
    PROCESS_INFORMATION info {};
    HANDLE job = nullptr;
};
#endif

struct TemporaryScanResult
{
    juce::File file = juce::File::getSpecialLocation (juce::File::tempDirectory)
        .getChildFile ("ForkHostScan-" + juce::Uuid().toString() + ".xml");

    ~TemporaryScanResult() { file.deleteFile(); }
};
}

PluginScanLog::PluginScanLog (juce::File file)
    : reportFile (std::move (file)),
      stackReportFile (reportFile.getSiblingFile ("PluginScanStackTraces.log"))
{
}

void PluginScanLog::beginScan (const juce::StringArray& previouslyBlacklisted)
{
    const juce::ScopedLock scopedLock (lock);
    entryCount = 0;
    appendLine ("\n=== Plug-in scan " + juce::Time::getCurrentTime().toISO8601 (true) + " ===");

    for (const auto& path : previouslyBlacklisted)
    {
        appendLine ("Previously blacklisted; skipped this scan");
        appendLine ("  " + path);
        ++entryCount;
    }
}

void PluginScanLog::recordFailure (const juce::String& format,
                                  const juce::String& fileOrIdentifier,
                                  const juce::String& reason,
                                  const juce::String& threadStacks)
{
    const juce::ScopedLock scopedLock (lock);
    appendLine ("Failed plug-in scan (" + format + "): " + reason);
    appendLine ("  " + fileOrIdentifier);
    ++entryCount;

    stackReportFile.appendText ("\n=== Failed helper "
        + juce::Time::getCurrentTime().toISO8601 (true) + " ===" + juce::newLine
        + "Format: " + format + juce::newLine
        + "Plug-in: " + fileOrIdentifier + juce::newLine
        + "Failure: " + reason + juce::newLine
        + (threadStacks.isNotEmpty() ? threadStacks
                                     : "No live helper stack was available for this failure.")
        + juce::newLine,
        false);
}

void PluginScanLog::finishScan()
{
    const juce::ScopedLock scopedLock (lock);
    appendLine ("Scan finished; " + juce::String (entryCount) + " failed or skipped plug-in(s).");
}

bool PluginScanLog::clearResults()
{
    const juce::ScopedLock scopedLock (lock);
    entryCount = 0;
    const auto failuresCleared = reportFile.deleteFile() || ! reportFile.existsAsFile();
    const auto stacksCleared = stackReportFile.deleteFile() || ! stackReportFile.existsAsFile();
    return failuresCleared && stacksCleared;
}

int PluginScanLog::getEntryCount() const
{
    const juce::ScopedLock scopedLock (lock);
    return entryCount;
}

juce::File PluginScanLog::getReportFile() const
{
    return reportFile;
}

void PluginScanLog::appendLine (const juce::String& line)
{
    reportFile.appendText (line + juce::newLine, false);
}

IsolatedPluginScanner::IsolatedPluginScanner (std::shared_ptr<PluginScanLog> logger)
    : scanLog (std::move (logger))
{
}

bool IsolatedPluginScanner::findPluginTypesFor (juce::AudioPluginFormat& format,
                                                 juce::OwnedArray<juce::PluginDescription>& result,
                                                 const juce::String& fileOrIdentifier)
{
    TemporaryScanResult scanResult;

    juce::StringArray command;
    command.add (juce::File::getSpecialLocation (juce::File::currentExecutableFile).getFullPathName());
    command.add (helperArgument);
    command.add (format.getName());
    command.add (fileOrIdentifier);
    command.add (scanResult.file.getFullPathName());

#if JUCE_WINDOWS
    ScannerHelperProcess child;
    const auto childStarted = child.start (command);
#else
    juce::ChildProcess child;
    const auto childStarted = child.start (command, 0);
#endif
    if (! childStarted)
    {
        if (scanLog != nullptr)
            scanLog->recordFailure (format.getName(), fileOrIdentifier,
                                    "could not start the scanner helper process");
        return false;
    }

    const auto startTime = juce::Time::getMillisecondCounter();
    auto* messageManager = juce::MessageManager::getInstanceWithoutCreating();

    while (child.isRunning())
    {
        if (child.waitForProcessToFinish (20))
            break;

        if (messageManager != nullptr && messageManager->isThisTheMessageThread())
            messageManager->runDispatchLoopUntil (1);

        if (juce::Time::getMillisecondCounter() - startTime >= (juce::uint32) scanTimeoutMs)
        {
            juce::String stacks;
#if JUCE_WINDOWS
            stacks = child.getThreadStacks();
#endif
            child.kill();
            child.waitForProcessToFinish (2000);
            if (scanLog != nullptr)
                scanLog->recordFailure (format.getName(), fileOrIdentifier,
                                        "scanner helper timed out after 30 seconds", stacks);
            return false;
        }

        if (shouldExit())
        {
            juce::String stacks;
#if JUCE_WINDOWS
            stacks = child.getThreadStacks();
#endif
            child.kill();
            child.waitForProcessToFinish (2000);
            if (scanLog != nullptr)
                scanLog->recordFailure (format.getName(), fileOrIdentifier,
                                        "scan was cancelled", stacks);
            return false;
        }
    }

    if (child.getExitCode() != 0)
    {
        if (scanLog != nullptr)
            scanLog->recordFailure (format.getName(), fileOrIdentifier,
                                    "scanner helper exited with code 0x"
                                        + juce::String::toHexString ((int) child.getExitCode()));
        return false;
    }

    if (! scanResult.file.existsAsFile())
    {
        if (scanLog != nullptr)
            scanLog->recordFailure (format.getName(), fileOrIdentifier,
                                    "scanner helper did not produce a result file");
        return false;
    }

    auto xml = juce::XmlDocument::parse (scanResult.file);
    if (xml == nullptr || ! xml->hasTagName ("PluginScanResult"))
    {
        if (scanLog != nullptr)
            scanLog->recordFailure (format.getName(), fileOrIdentifier,
                                    "scanner helper returned an invalid result");
        return false;
    }

    for (auto* element = xml->getFirstChildElement(); element != nullptr;
         element = element->getNextElement())
    {
        auto description = std::make_unique<juce::PluginDescription>();
        if (description->loadFromXml (*element))
            result.add (description.release());
    }

    // An empty, valid result means this file did not contain a plug-in of this
    // format. Crashes and hangs are reported as failures and get blacklisted by JUCE.
    return true;
}

bool isPluginScanHelperCommandLine (const juce::StringArray& arguments)
{
    return arguments.size() == 4 && arguments[0] == helperArgument;
}

int runPluginScanHelper (const juce::StringArray& arguments)
{
    if (! isPluginScanHelperCommandLine (arguments))
        return 2;

   #if JUCE_WINDOWS
    // A misbehaving plug-in must not leave a Windows crash dialog blocking the
    // parent scanner after this helper process has already failed.
    SetErrorMode (GetErrorMode() | SEM_NOGPFAULTERRORBOX | SEM_FAILCRITICALERRORS
                  | SEM_NOOPENFILEERRORBOX);
   #endif

    juce::AudioPluginFormatManager formatManager;
   #if JUCE_VERSION >= 0x080009
    juce::addDefaultFormatsToManager (formatManager);
   #else
    formatManager.addDefaultFormats();
   #endif

    juce::AudioPluginFormat* requestedFormat = nullptr;
    for (int i = 0; i < formatManager.getNumFormats(); ++i)
        if (auto* format = formatManager.getFormat (i))
            if (format->getName() == arguments[1])
                requestedFormat = format;

    if (requestedFormat == nullptr)
        return 3;

    juce::OwnedArray<juce::PluginDescription> descriptions;
    requestedFormat->findAllTypesForFile (descriptions, arguments[2]);

    juce::XmlElement resultXml ("PluginScanResult");
    for (auto* description : descriptions)
        if (description != nullptr)
            if (auto descriptionXml = description->createXml())
                resultXml.addChildElement (descriptionXml.release());

    juce::File outputFile (arguments[3]);
    juce::FileOutputStream output (outputFile);
    if (! output.openedOk())
        return 4;

    resultXml.writeTo (output);
    output.flush();
    return output.getStatus().failed() ? 5 : 0;
}
