#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <tracktion_engine/tracktion_engine.h>

#include "RiffLibrary.h"

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>

namespace te = tracktion::engine;

//==============================================================================
// Localhost JSON-RPC 2.0 bridge for the AI co-producer.
//
// A reader thread accepts newline-delimited UTF-8 frames and queues them; the
// JUCE message thread drains the queue and applies every Edit read/mutation,
// so no engine state is ever touched from the socket thread. In GUI mode the
// drain is triggered with MessageManager::callAsync; --smoke-test pumps
// servicePendingRequestsOnMessageThread() directly.
//==============================================================================
class DawIpcServer : private juce::Thread
{
public:
    explicit DawIpcServer (RiffLibrary& targetRiffLibrary);
    ~DawIpcServer() override;

    void setEdit (te::Edit* newEdit);

    bool startListening (int listenPort = 9450);
    void stopListening();

    bool isListening() const noexcept { return listening.load (std::memory_order_acquire); }
    int getBoundPort() const noexcept { return boundPort.load (std::memory_order_acquire); }

    // Executes a single JSON-RPC 2.0 payload. Call on the JUCE message thread.
    juce::String executeJsonRpcOnMessageThread (const juce::String& rawJson);

    // Drains queued TCP requests on the JUCE message thread.
    void servicePendingRequestsOnMessageThread();

    std::function<void (const juce::String& /*summary*/)> onEditMutatedByRpc;

private:
    struct PendingRpcRequest
    {
        juce::String rawJson;
        std::shared_ptr<std::promise<juce::String>> responsePromise;
    };

    void run() override;
    void handleClient (juce::StreamingSocket& client);
    juce::String enqueueAndWaitForMessageThread (const juce::String& rawJson);

    juce::String dispatchMethod (const juce::var& id,
                                 const juce::String& method,
                                 const juce::var& params);

    static int getInsertIndexBeforeTerminalPlugins (te::AudioTrack& track);
    static juce::String makeResultResponse (const juce::var& id, const juce::var& result);
    static juce::String makeErrorResponse (const juce::var& id, int code, const juce::String& msg);

    RiffLibrary& riffLibrary;
    te::Edit* edit = nullptr;

    juce::StreamingSocket serverSocket;

    std::atomic<bool> listening { false };
    std::atomic<int> boundPort { 0 };

    std::mutex queueMutex;
    std::deque<PendingRpcRequest> pendingQueue;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (DawIpcServer)
};
