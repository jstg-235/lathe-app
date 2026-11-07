// Out-of-process plugin scanning. Each plugin is examined in a helper process (Lathe itself,
// started with a special command line). A plugin that crashes or hangs during the scan only
// takes the helper down: it is put on the blacklist, a fresh helper starts, and the scan
// carries on. The app never crashes because of a scan.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace lathe::scan {

// True when this process was started as a scan helper.
bool isWorkerCommandLine(const juce::String& commandLine);

// If the command line asks for it, turns this process into a scan helper and returns the
// object that must be kept alive for the life of the process; otherwise returns null.
// `addFormats` registers the plugin formats the helper can scan.
std::unique_ptr<juce::ChildProcessWorker> startWorkerIfRequested(const juce::String& commandLine,
                                                                 std::function<void(juce::AudioPluginFormatManager&)> addFormats);

// The scanner to install with KnownPluginList::setCustomScanner. A plugin that doesn't answer
// within `timeoutMs` counts as hung. `executable` is the helper to launch (default: this app).
std::unique_ptr<juce::KnownPluginList::CustomScanner> makeOutOfProcessScanner(int timeoutMs = 120000, juce::File executable = {});

}  // namespace lathe::scan
