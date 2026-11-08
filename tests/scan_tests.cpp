// Out-of-process plugin scanning: a plugin that crashes or hangs while being examined must be
// blacklisted while the scan (and this process) carries on. Uses a fake plugin format whose
// "plugins" are named for what they do; this test binary doubles as the scan helper.
#include "../app/Source/PluginScan.h"

#if JUCE_MAC
#include <CoreFoundation/CoreFoundation.h>
#endif
#include <cstdio>
#include <cstdlib>
#include <thread>

static int checks = 0, failures = 0;
#define CHECK(c) do { ++checks; if (!(c)) { ++failures; std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

namespace {

class FakeFormat : public juce::AudioPluginFormat {
public:
    explicit FakeFormat(juce::String name = "LatheTest", juce::StringArray items = {"good-one", "crash-me", "good-two", "hang-me", "abort-me", "good-three"})
        : name_(std::move(name)), items_(std::move(items)) {}
    juce::String getName() const override { return name_; }
    void findAllTypesForFile(juce::OwnedArray<juce::PluginDescription>& out, const juce::String& id) override {
        if (id.contains("crash")) { volatile int* p = nullptr; *p = 42; }        // segfault, like a broken plugin
        if (id.contains("abort")) std::abort();
        if (id.contains("hang")) for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
        auto d = std::make_unique<juce::PluginDescription>();
        d->name = id;
        d->pluginFormatName = getName();
        d->fileOrIdentifier = id;
        d->manufacturerName = "Test";
        d->uniqueId = d->deprecatedUid = int(id.hashCode());
        out.add(std::move(d));
    }
    bool fileMightContainThisPluginType(const juce::String&) override { return true; }
    juce::String getNameOfPluginFromIdentifier(const juce::String& id) override { return id; }
    bool pluginNeedsRescanning(const juce::PluginDescription&) override { return false; }
    bool doesPluginStillExist(const juce::PluginDescription&) override { return true; }
    bool canScanForPlugins() const override { return true; }
    bool isTrivialToScan() const override { return false; }
    juce::StringArray searchPathsForPlugins(const juce::FileSearchPath&, bool, bool) override {
        return items_;
    }
    juce::FileSearchPath getDefaultLocationsToSearch() override { return {}; }
    bool requiresUnblockedMessageThreadDuringCreation(const juce::PluginDescription&) const override { return false; }
private:
    void createPluginInstance(const juce::PluginDescription&, double, int, PluginCreationCallback cb) override { cb(nullptr, "fake"); }
    juce::String name_;
    juce::StringArray items_;
};

}  // namespace

int main(int argc, char** argv) {
    juce::ScopedJuceInitialiser_GUI gui;
    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add(argv[i]);
    const auto cmd = args.joinIntoString(" ");

    // Helper role: serve scan requests until the coordinator goes away.
    if (lathe::scan::isWorkerCommandLine(cmd)) {
        auto w = lathe::scan::startWorkerIfRequested(cmd, [](juce::AudioPluginFormatManager& f) { f.addFormat(new FakeFormat()); });
        if (!w) return 2;
#if JUCE_MAC
        // Pump the main run loop directly (a console process has no running NSApplication).
        for (;;) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
#else
        juce::MessageManager::getInstance()->runDispatchLoop();
#endif
        return 0;
    }

    // Smoke test with another helper binary (the Lathe app): `lathe_scan_tests --helper <exe>`.
    // Asks it to scan VST3 bundles that don't exist: each request goes through the helper's real
    // VST3 format on its message thread and must come back empty, quickly, none skipped.
    if (args.size() == 2 && args[0] == "--helper") {
        std::thread t([&] {
            juce::KnownPluginList list;
            list.setCustomScanner(lathe::scan::makeOutOfProcessScanner(15000, juce::File(args[1])));
            FakeFormat format("VST3", {"/nonexistent/One.vst3", "/nonexistent/Two.vst3", "/nonexistent/Three.vst3"});
            juce::PluginDirectoryScanner scanner(list, format, {}, true, {});
            juce::String name;
            int n = 0;
            while (scanner.scanNextFile(true, name)) ++n;
            list.scanFinished();
            std::printf("external helper: %d items, %d found, %d skipped\n", n, list.getNumTypes(), list.getBlacklistedFiles().size());
            CHECK(list.getBlacklistedFiles().isEmpty() && list.getNumTypes() == 0);
            juce::MessageManager::callAsync([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
        });
        juce::MessageManager::getInstance()->runDispatchLoop();
        t.join();
        std::printf("%d checks, %d failures\n", checks, failures);
        return failures ? 1 : 0;
    }

    // Coordinator role: scan everything through the helper.
    std::thread scanThread([&] {
        juce::KnownPluginList list;
        list.setCustomScanner(lathe::scan::makeOutOfProcessScanner(3000, juce::File::getSpecialLocation(juce::File::currentExecutableFile)));
        FakeFormat format;
        auto pedal = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lathe-scan-test-pedal.txt");
        pedal.deleteFile();
        juce::PluginDirectoryScanner scanner(list, format, {}, true, pedal);
        const auto t0 = juce::Time::getMillisecondCounterHiRes();
        juce::String name;
        while (scanner.scanNextFile(true, name)) {}
        const double secs = (juce::Time::getMillisecondCounterHiRes() - t0) / 1000.0;
        list.scanFinished();

        juce::StringArray found;
        for (auto& d : list.getTypes()) found.add(d.name);
        found.sort(true);
        auto black = list.getBlacklistedFiles();
        black.sort(true);
        std::printf("found: %s\nskipped: %s\ntime: %.1f s\n", found.joinIntoString(", ").toRawUTF8(), black.joinIntoString(", ").toRawUTF8(), secs);
        CHECK(found == juce::StringArray({"good-one", "good-three", "good-two"}));
        CHECK(black == juce::StringArray({"abort-me", "crash-me", "hang-me"}));
        CHECK(secs < 20.0);   // the hang costs the 3 s timeout, not forever
        // No helper may be left behind, including the one a plugin hung.
        juce::Thread::sleep(1500);
        juce::ChildProcess ps;
        juce::String left;
        if (ps.start("pgrep -f latheScanWorker")) left = ps.readAllProcessOutput().trim();
        if (left.isNotEmpty()) std::printf("leftover helpers: %s\n", left.toRawUTF8());
        CHECK(left.isEmpty());
        juce::MessageManager::callAsync([] { juce::MessageManager::getInstance()->stopDispatchLoop(); });
    });
    juce::MessageManager::getInstance()->runDispatchLoop();
    scanThread.join();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
