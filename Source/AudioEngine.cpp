#include "AudioEngine.h"
#include <cmath>

namespace
{
bool configureEffect(juce::AudioPluginInstance& effect)
{
    using Channels = juce::AudioChannelSet;
    effect.disableNonMainBuses();

    const auto stereo = Channels::stereo();
    const auto mono = Channels::mono();
    for (const auto& [input, output] : { std::pair { stereo, stereo }, std::pair { mono, mono },
                                         std::pair { mono, stereo }, std::pair { stereo, mono } })
    {
        auto layout = effect.getBusesLayout();
        if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty())
            return false;

        layout.inputBuses.set(0, input);
        layout.outputBuses.set(0, output);
        if (effect.setBusesLayout(layout)
            && effect.getTotalNumInputChannels() <= 2 && effect.getTotalNumOutputChannels() <= 2)
            return true;
    }

    return false;
}

float absolutePeak(const float* samples, int frames)
{
    const auto range = juce::FloatVectorOperations::findMinAndMax(samples, frames);
    return juce::jmax(std::abs(range.getStart()), std::abs(range.getEnd()));
}
}

AudioEngine::AudioEngine() : scratch(2, 512)
{
    juce::addDefaultFormatsToManager(formats);
    midi.ensureSize(4096);
}

AudioEngine::~AudioEngine()
{
    devices.removeAudioCallback(this);
    for (auto& effect : chain)
        if (effect.prepared)
            effect.instance->releaseResources();
}

const AudioEngine::Effect* AudioEngine::getEffect(int index) const
{
    return juce::isPositiveAndBelow(index, getNumEffects()) ? &chain[static_cast<size_t>(index)] : nullptr;
}

int AudioEngine::getNumEffects() const
{
    return static_cast<int>(chain.size());
}

bool AudioEngine::loadEffect(Effect& effect, juce::String& error)
{
    double rate;
    int size;
    {
        const juce::ScopedLock lock(chainLock);
        rate = sampleRate;
        size = blockSize;
    }

    effect.instance = formats.createPluginInstance(effect.description, rate, size, error);
    if (effect.instance == nullptr)
        return false;

    if (!effect.lastState.isEmpty())
        effect.instance->setStateInformation(effect.lastState.getData(),
                                             static_cast<int>(effect.lastState.getSize()));

    if (!configureEffect(*effect.instance))
    {
        error = "This plug-in requires a channel layout other than mono or stereo.";
        effect.instance.reset();
        return false;
    }

    return true;
}

void AudioEngine::prepareEffect(Effect& effect)
{
    if (effect.instance != nullptr)
    {
        effect.instance->setRateAndBufferSizeDetails(sampleRate, blockSize);
        effect.instance->prepareToPlay(sampleRate, blockSize);
        effect.prepared = true;
    }
}

bool AudioEngine::addEffect(const juce::PluginDescription& description, juce::String& error)
{
    Effect effect;
    effect.description = description;
    if (!loadEffect(effect, error))
        return false;

    const juce::ScopedLock lock(chainLock);
    prepareEffect(effect);
    chain.push_back(std::move(effect));
    return true;
}

void AudioEngine::removeEffect(int index)
{
    Effect removed;
    {
        const juce::ScopedLock lock(chainLock);
        auto position = chain.begin() + index;
        removed = std::move(*position);
        chain.erase(position);
    }
    if (removed.prepared)
        removed.instance->releaseResources();
}

void AudioEngine::moveEffect(int from, int to)
{
    const juce::ScopedLock lock(chainLock);
    std::swap(chain[static_cast<size_t>(from)], chain[static_cast<size_t>(to)]);
}

void AudioEngine::toggleEffect(int index)
{
    const juce::ScopedLock lock(chainLock);
    auto& effect = chain[static_cast<size_t>(index)];
    effect.enabled = !effect.enabled;
}

std::unique_ptr<juce::XmlElement> AudioEngine::saveChain()
{
    auto xml = std::make_unique<juce::XmlElement>("Effects");
    const juce::ScopedLock lock(chainLock);
    for (auto& effect : chain)
    {
        if (effect.instance != nullptr)
            effect.instance->getStateInformation(effect.lastState);

        auto* item = xml->createNewChildElement("Effect");
        item->setAttribute("enabled", effect.enabled);
        item->setAttribute("state", effect.lastState.toBase64Encoding());
        item->addChildElement(effect.description.createXml().release());
    }
    return xml;
}

juce::StringArray AudioEngine::restoreChain(const juce::XmlElement& xml)
{
    juce::StringArray errors;
    for (auto* item : xml.getChildWithTagNameIterator("Effect"))
    {
        Effect effect;
        if (auto* description = item->getChildByName("PLUGIN"); description == nullptr
            || !effect.description.loadFromXml(*description))
            continue;

        effect.enabled = item->getBoolAttribute("enabled", true);
        effect.lastState.fromBase64Encoding(item->getStringAttribute("state"));
        juce::String error;
        if (!loadEffect(effect, error))
            errors.add(effect.description.name + ": " + error);

        const juce::ScopedLock lock(chainLock);
        prepareEffect(effect);
        chain.push_back(std::move(effect));
    }
    return errors;
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    const juce::ScopedLock lock(chainLock);
    sampleRate = device->getCurrentSampleRate();
    blockSize = juce::jmax(1, device->getCurrentBufferSizeSamples());
    scratch.setSize(2, blockSize);

    for (auto& effect : chain)
    {
        if (effect.prepared)
            effect.instance->releaseResources();
        prepareEffect(effect);
    }

    inputCount.store(device->getActiveInputChannels().countNumberOfSetBits());
    outputCount.store(device->getActiveOutputChannels().countNumberOfSetBits());
    streamReady.store(inputCount.load() > 0 && outputCount.load() > 0);
}

void AudioEngine::audioDeviceStopped()
{
    if (running.exchange(false))
        interrupted.store(true);
    streamReady.store(false);
    inputCount.store(0);
    outputCount.store(0);
    peakLevels.store({}, std::memory_order_relaxed);
    const juce::ScopedLock lock(chainLock);
    for (auto& effect : chain)
    {
        if (effect.prepared)
        {
            effect.instance->releaseResources();
            effect.prepared = false;
        }
    }
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs,
                                                    float* const* outputs, int numOutputs,
                                                    int frames, const juce::AudioIODeviceCallbackContext&)
{
    const float* selectedInputs[2] {};
    int selectedInputCount = 0;
    for (int channel = 0; channel < numInputs && selectedInputCount < 2; ++channel)
        if (inputs[channel] != nullptr)
            selectedInputs[selectedInputCount++] = inputs[channel];

    float* selectedOutputs[2] {};
    int selectedOutputCount = 0;
    for (int channel = 0; channel < numOutputs; ++channel)
        if (outputs[channel] != nullptr)
        {
            juce::FloatVectorOperations::clear(outputs[channel], frames);
            if (selectedOutputCount < 2)
                selectedOutputs[selectedOutputCount++] = outputs[channel];
        }

    const auto* left = selectedInputs[0];
    const auto* right = selectedInputCount == 2 ? selectedInputs[1] : left;
    if (running.load() && left != nullptr && frames > 0)
        render(left, right, selectedOutputs, selectedOutputCount, frames);

    if (frames > 0)
    {
        recordPeaks(left, right, selectedOutputs, selectedOutputCount, frames);
        processedBlocks.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioEngine::render(const float* left, const float* right, float* const* outputs,
                         int numOutputs, int frames)
{
    if (numOutputs == 0)
        return;

    const juce::ScopedTryLock lock(chainLock);
    if (lock.isLocked() && !bypass.load() && frames <= scratch.getNumSamples())
    {
        bool processed = false;
        for (const auto& effect : chain)
        {
            if (!effect.enabled || !effect.prepared)
                continue;

            if (!processed)
            {
                scratch.copyFrom(0, 0, left, frames);
                scratch.copyFrom(1, 0, right, frames);
                processed = true;
            }

            const auto inputs = effect.instance->getTotalNumInputChannels();
            const auto channels = effect.instance->getTotalNumOutputChannels();
            if (inputs == 1)
            {
                scratch.addFrom(0, 0, scratch, 1, 0, frames);
                scratch.applyGain(0, 0, frames, 0.5f);
                scratch.clear(1, 0, frames);
            }

            juce::AudioBuffer<float> block(scratch.getArrayOfWritePointers(),
                                          juce::jmax(inputs, channels), frames);
            midi.clear();
            effect.instance->processBlock(block, midi);
            if (channels == 1)
                scratch.copyFrom(1, 0, scratch, 0, 0, frames);
        }

        if (processed)
        {
            left = scratch.getReadPointer(0);
            right = scratch.getReadPointer(1);
        }
    }

    if (outputs[0] != nullptr)
    {
        if (numOutputs == 1 && left != right)
        {
            juce::FloatVectorOperations::add(outputs[0], left, right, frames);
            juce::FloatVectorOperations::multiply(outputs[0], 0.5f, frames);
        }
        else
        {
            juce::FloatVectorOperations::copy(outputs[0], left, frames);
        }
    }

    if (numOutputs > 1 && outputs[1] != nullptr)
        juce::FloatVectorOperations::copy(outputs[1], right, frames);
}

void AudioEngine::recordPeaks(const float* left, const float* right, float* const* outputs,
                               int numOutputs, int frames)
{
    PeakLevels measured;
    if (left != nullptr)
        measured.input = juce::jmax(absolutePeak(left, frames), absolutePeak(right, frames));
    for (int channel = 0; channel < numOutputs; ++channel)
        if (outputs[channel] != nullptr)
            measured.output = juce::jmax(measured.output, absolutePeak(outputs[channel], frames));
    peakLevels.store(measured, std::memory_order_relaxed);
}
