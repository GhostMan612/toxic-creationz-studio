#pragma once

// Desktop-only localhost Ollama client. All socket I/O happens on a background
// thread; replies are marshalled back to the JUCE message thread by the caller.

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <thread>

class GhostBrain
{
public:
    struct Reply
    {
        juce::String text;
        bool fromOllama = false;
        juce::String detail;
    };

    using Callback = std::function<void (const Reply&)>;

    GhostBrain() = default;
    ~GhostBrain();

    void requestAsync (const juce::String& question,
                       const juce::String& prompt,
                       bool allowLlm,
                       Callback callback);

    void cancel();

    static void setHeadless (bool value) noexcept { headless.store (value, std::memory_order_release); }
    static bool isHeadless() noexcept { return headless.load (std::memory_order_acquire); }

    bool wasReachable() const noexcept { return reachable.load (std::memory_order_relaxed); }
    void setModel (const juce::String& m) { model = m; }
    juce::String getModel() const { return model; }

private:
    void run (juce::String question, juce::String prompt, Callback callback);

    std::thread worker;
    static std::atomic<bool> headless;
    std::atomic<bool> cancelled { false };
    std::atomic<bool> reachable { false };
    juce::String model { "qwen2.5:3b" };
};
