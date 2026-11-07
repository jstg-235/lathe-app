#include "PluginScan.h"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <thread>

#if JUCE_MAC || JUCE_LINUX
#include <csignal>
#include <unistd.h>
#endif

namespace lathe::scan {

namespace {

constexpr const char* kWorkerUID = "latheScanWorker";
constexpr const char* kPidPrefix = "LATHE_PID:";

// ------------------------------------------------------------------ helper side

class Worker final : public juce::ChildProcessWorker, private juce::AsyncUpdater {
public:
    explicit Worker(std::function<void(juce::AudioPluginFormatManager&)> addFormats) {
        if (addFormats) addFormats(formats_);
        else formats_.addDefaultFormats();
#if JUCE_MAC || JUCE_LINUX
        // If Lathe goes away while a plugin has this helper stuck, leave anyway.
        parent_ = getppid();
        watchdog_ = std::thread([this] {
            while (!stop_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                if (getppid() != parent_) juce::Process::terminate();
            }
        });
#endif
    }
    ~Worker() override {
        stop_ = true;
        if (watchdog_.joinable()) watchdog_.join();
    }

private:
    // Tell the app our process id so it can stop us if a plugin hangs this helper: the normal
    // "quit" request is delivered on the message thread, which a hung plugin is holding.
    void handleConnectionMade() override {
#if JUCE_MAC || JUCE_LINUX
        const auto s = juce::String(kPidPrefix) + juce::String(int(getpid()));
        sendMessageToCoordinator({s.toRawUTF8(), s.getNumBytesAsUTF8()});
#endif
    }

    void handleMessageFromCoordinator(const juce::MemoryBlock& mb) override {
        if (mb.isEmpty()) return;
        const std::lock_guard<std::mutex> lock(mutex_);
        juce::MemoryInputStream in(mb, false);
        const auto formatName = in.readString();
        const auto identifier = in.readString();
        auto* format = findFormat(formatName);
        if (format == nullptr) { send({}); return; }
        juce::PluginDescription pd;
        pd.fileOrIdentifier = identifier;
        // Formats that need the message thread free (AUv3) are scanned right here; everything
        // else is scanned on the message thread, which is what plugins expect.
        if (format->requiresUnblockedMessageThreadDuringCreation(pd)) {
            juce::OwnedArray<juce::PluginDescription> found;
            format->findAllTypesForFile(found, identifier);
            send(found);
            return;
        }
        pending_.push({formatName, identifier});
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override {
        for (;;) {
            std::pair<juce::String, juce::String> job;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                if (pending_.empty()) return;
                job = pending_.front();
                pending_.pop();
            }
            juce::OwnedArray<juce::PluginDescription> found;
            if (auto* format = findFormat(job.first)) format->findAllTypesForFile(found, job.second);
            send(found);
        }
    }

    // The coordinator went away or told us to stop. Leave at once: a plugin may be holding the
    // message thread, so a polite quit could never run.
    void handleConnectionLost() override { juce::Process::terminate(); }

    juce::AudioPluginFormat* findFormat(const juce::String& name) {
        for (auto* f : formats_.getFormats())
            if (f->getName() == name) return f;
        return nullptr;
    }

    void send(const juce::OwnedArray<juce::PluginDescription>& found) {
        juce::XmlElement xml("LIST");
        for (auto* d : found) xml.addChildElement(d->createXml().release());
        const auto s = xml.toString();
        sendMessageToCoordinator({s.toRawUTF8(), s.getNumBytesAsUTF8()});
    }

    juce::AudioPluginFormatManager formats_;
    std::mutex mutex_;
    std::queue<std::pair<juce::String, juce::String>> pending_;
    std::atomic<bool> stop_{false};
    std::thread watchdog_;
    int parent_ = 0;
};

// ------------------------------------------------------------------ app side

class Helper final : private juce::ChildProcessCoordinator {
public:
    explicit Helper(const juce::File& exe) {
        // No stdout/stderr pipes: a chatty plugin could fill an unread pipe and stall.
        launched_ = launchWorkerProcess(exe, kWorkerUID, 0, 0);
    }
    ~Helper() override { stop(false); }

    // Ends the helper. `force`: it may be stuck in a plugin, so don't rely on it cooperating.
    void stop(bool force) {
        killWorkerProcess();
#if JUCE_MAC || JUCE_LINUX
        if (force && pid_ > 0) ::kill(pid_, SIGKILL);
#endif
        pid_ = 0;
    }

    bool launched() const { return launched_; }
    bool send(const juce::MemoryBlock& mb) { return sendMessageToWorker(mb); }

    enum class State { waiting, gotResult, lost };
    State wait(int ms, std::unique_ptr<juce::XmlElement>& xml) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!cv_.wait_for(lock, std::chrono::milliseconds(ms), [&] { return got_ || lost_; })) return State::waiting;
        const auto s = lost_ ? State::lost : State::gotResult;
        got_ = false;
        xml = std::move(result_);
        return s;
    }

private:
    void handleMessageFromWorker(const juce::MemoryBlock& mb) override {
        const auto text = mb.toString();
        if (text.startsWith(kPidPrefix)) { pid_ = text.fromFirstOccurrenceOf(kPidPrefix, false, false).getIntValue(); return; }
        const std::lock_guard<std::mutex> lock(mutex_);
        result_ = juce::parseXML(text);
        got_ = true;
        cv_.notify_one();
    }
    void handleConnectionLost() override {
        const std::lock_guard<std::mutex> lock(mutex_);
        lost_ = true;
        cv_.notify_one();
    }

    bool launched_ = false;
    std::atomic<int> pid_{0};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::unique_ptr<juce::XmlElement> result_;
    bool got_ = false, lost_ = false;
};

class OutOfProcessScanner final : public juce::KnownPluginList::CustomScanner {
public:
    OutOfProcessScanner(int timeoutMs, juce::File exe)
        : timeoutMs_(timeoutMs), exe_(exe == juce::File() ? juce::File::getSpecialLocation(juce::File::currentExecutableFile) : exe) {}

    bool findPluginTypesFor(juce::AudioPluginFormat& format, juce::OwnedArray<juce::PluginDescription>& result,
                            const juce::String& fileOrIdentifier) override {
        const std::lock_guard<std::mutex> lock(scanMutex_);   // one plugin at a time per helper
        if (!helper_) helper_ = std::make_unique<Helper>(exe_);
        if (!helper_->launched()) {
            // No helper available (shouldn't happen): fall back to scanning here.
            helper_.reset();
            format.findAllTypesForFile(result, fileOrIdentifier);
            return true;
        }
        juce::MemoryBlock mb;
        {
            juce::MemoryOutputStream out(mb, false);
            out.writeString(format.getName());
            out.writeString(fileOrIdentifier);
        }
        if (!helper_->send(mb)) { helper_.reset(); return false; }
        const auto start = juce::Time::getMillisecondCounterHiRes();
        for (;;) {
            if (shouldExit()) {   // user cancelled: not the plugin's fault, but it may be stuck
                helper_->stop(true);
                helper_.reset();
                return true;
            }
            std::unique_ptr<juce::XmlElement> xml;
            const auto state = helper_->wait(50, xml);
            if (state == Helper::State::waiting) {
                if (juce::Time::getMillisecondCounterHiRes() - start > timeoutMs_) {   // hung
                    helper_->stop(true);
                    helper_.reset();
                    return false;
                }
                continue;
            }
            if (state == Helper::State::lost) { helper_.reset(); return false; }   // crashed
            if (xml)
                for (auto* item : xml->getChildIterator()) {
                    auto d = std::make_unique<juce::PluginDescription>();
                    if (d->loadFromXml(*item)) result.add(std::move(d));
                }
            return true;
        }
    }

    void scanFinished() override {
        const std::lock_guard<std::mutex> lock(scanMutex_);
        helper_.reset();
    }

private:
    const int timeoutMs_;
    const juce::File exe_;
    std::mutex scanMutex_;
    std::unique_ptr<Helper> helper_;
};

}  // namespace

bool isWorkerCommandLine(const juce::String& commandLine) { return commandLine.contains(juce::String("--") + kWorkerUID + ":"); }

std::unique_ptr<juce::ChildProcessWorker> startWorkerIfRequested(const juce::String& commandLine,
                                                                 std::function<void(juce::AudioPluginFormatManager&)> addFormats) {
    if (!isWorkerCommandLine(commandLine)) return nullptr;
    auto w = std::make_unique<Worker>(std::move(addFormats));
    if (!w->initialiseFromCommandLine(commandLine, kWorkerUID)) return nullptr;
    return w;
}

std::unique_ptr<juce::KnownPluginList::CustomScanner> makeOutOfProcessScanner(int timeoutMs, juce::File executable) {
    return std::make_unique<OutOfProcessScanner>(timeoutMs, executable);
}

}  // namespace lathe::scan
