#include "GhostBrain.h"

#include "GhostKnowledge.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>

std::atomic<bool> GhostBrain::headless { false };

GhostBrain::~GhostBrain()
{
    cancel();
}

void GhostBrain::cancel()
{
    cancelled.store (true, std::memory_order_release);
    if (worker.joinable())
        worker.join();
}

void GhostBrain::requestAsync (const juce::String& question,
                               const juce::String& prompt,
                               bool allowLlm,
                               Callback callback)
{
    cancel();
    cancelled.store (false, std::memory_order_release);

    if (! allowLlm || isHeadless())
    {
        const auto knowledge = GhostKnowledge::answer (question);
        const Reply reply { knowledge.text, false, knowledge.sectionId };
        juce::MessageManager::callAsync ([callback, reply] { callback (reply); });
        return;
    }

    worker = std::thread ([this, question, prompt, callback] () mutable
    {
        run (question, prompt, callback);
    });
}

static juce::String jsonEscaped (const juce::String& raw)
{
    juce::String out;
    for (auto ch : raw)
    {
        if (ch == '"' || ch == '\\')
            out << '\\' << ch;
        else if (ch == '\n')
            out << "\\n";
        else if (ch == '\r')
            out << "\\r";
        else
            out << ch;
    }
    return out;
}

void GhostBrain::run (juce::String question, juce::String prompt, Callback callback)
{
    juce::String responseText;
    bool ok = false;

    for (int attempt = 0; attempt < 2 && ! ok && ! cancelled.load (std::memory_order_acquire); ++attempt)
    {
        juce::StreamingSocket socket;
        if (! socket.connect ("127.0.0.1", 11450, 1500))
            continue;

        reachable.store (true, std::memory_order_release);
        const auto body = "{\"model\":\"" + jsonEscaped (model) + "\","
                          "\"stream\":false,\"messages\":["
                          "{\"role\":\"system\",\"content\":\"" + jsonEscaped (prompt) + "\"},"
                          "{\"role\":\"user\",\"content\":\"" + jsonEscaped (question) + "\"}]}";
        const auto request = juce::String ("POST /api/chat HTTP/1.1\r\n")
                           + "Host: 127.0.0.1:11450\r\n"
                           + "Content-Type: application/json\r\n"
                           + "Content-Length: " + juce::String (body.getNumBytesAsUTF8()) + "\r\n"
                           + "Connection: close\r\n\r\n" + body;
        socket.write (request.toRawUTF8(), static_cast<int> (request.getNumBytesAsUTF8()));

        juce::MemoryBlock block;
        char buffer[8192];
        while (! cancelled.load (std::memory_order_acquire)
               && socket.waitUntilReady (true, 30000) > 0)
        {
            const int read = socket.read (buffer, static_cast<int> (sizeof (buffer)), false);
            if (read <= 0)
                break;
            block.append (buffer, static_cast<size_t> (read));
        }

        const juce::String raw (block.toString());
        const auto jsonStart = raw.indexOfChar ('{');
        if (jsonStart < 0)
            continue;

        juce::var parsed;
        if (! juce::JSON::parse (raw.substring (jsonStart), parsed).wasOk())
            continue;
        const auto content = parsed["message"]["content"].toString();
        if (content.isNotEmpty())
        {
            responseText = content;
            ok = true;
        }
    }

    Reply reply;
    if (ok && ! cancelled.load (std::memory_order_acquire))
    {
        reply.text = responseText;
        reply.fromOllama = true;
    }
    else
    {
        const auto knowledge = GhostKnowledge::answer (question);
        reply.text = "Ollama is down — answering from the built-in guide. " + knowledge.text;
        reply.detail = knowledge.sectionId;
    }

    juce::MessageManager::callAsync ([callback, reply] { callback (reply); });
}
