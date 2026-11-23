#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>
#include <vector>

class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    struct PeakLevels
    {
        float input = 0.0f;
        float output = 0.0f;
    };

    struct Effect
    {
        juce::PluginDescription description;
        std::unique_ptr<juce::AudioPluginInstance> instance;
        juce::MemoryBlock lastState;
        bool enabled = true;
        bool prepared = false;
    };

    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager devices;
    juce::AudioPluginFormatManager formats;
    std::atomic<bool> running { false };
    std::atomic<bool> bypass { false };
    std::atomic<int> inputCount { 0 };
    std::atomic<int> outputCount { 0 };
    std::atomic<bool> streamReady { false };
    std::atomic<bool> interrupted { false };
    std::atomic<unsigned> processedBlocks { 0 };

    PeakLevels getPeakLevels() const noexcept { return peakLevels.load(std::memory_order_relaxed); }
    const Effect* getEffect(int index) const;
    int getNumEffects() const;
    bool addEffect(const juce::PluginDescription&, juce::String& error);
    void removeEffect(int index);
    void moveEffect(int from, int to);
    void toggleEffect(int index);
    std::unique_ptr<juce::XmlElement> saveChain();
    juce::StringArray restoreChain(const juce::XmlElement&);

    void audioDeviceAboutToStart(juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceIOCallbackWithContext(const float* const*, int, float* const*, int,
                                          int, const juce::AudioIODeviceCallbackContext&) override;

private:
    bool loadEffect(Effect&, juce::String& error);
    void prepareEffect(Effect&);
    void render(const float* left, const float* right, float* const* outputs,
                int numOutputs, int frames);
    void recordPeaks(const float* left, const float* right, float* const* outputs,
                     int numOutputs, int frames);

    juce::CriticalSection chainLock;
    static_assert(std::atomic<PeakLevels>::is_always_lock_free);
    std::atomic<PeakLevels> peakLevels { PeakLevels {} };
    std::vector<Effect> chain;
    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
    double sampleRate = 48000.0;
    int blockSize = 512;
};
