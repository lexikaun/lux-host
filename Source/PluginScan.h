#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>
#include <mutex>
#include <optional>
#include <thread>

class PluginScan
{
public:
    struct Result
    {
        std::vector<juce::PluginDescription> effects;
        juce::StringArray failures;
    };

    void start(juce::File path);
    void cancel();
    bool isRunning() const { return running.load(); }
    std::optional<Result> takeResult();

    static bool isPluginPath(const juce::File&);
    static int probe(const juce::File& file, const juce::File& resultFile);

private:
    std::atomic<bool> running { false };
    std::mutex mutex;
    std::optional<Result> result;
    std::jthread worker;
};
