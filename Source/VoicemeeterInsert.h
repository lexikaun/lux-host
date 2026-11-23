#pragma once

#include <juce_core/juce_core.h>

class VoicemeeterInsert
{
public:
    ~VoicemeeterInsert();

    static bool matchesDriver(const juce::String& type, const juce::String& name);
    juce::Result engage(const juce::BigInteger& inputs, const juce::BigInteger& outputs);
    juce::Result release();
    bool hasRoute() const { return !selected.isZero(); }
    bool returnsAreOn() const;

private:
    juce::Result connect();
    bool sendPatch(const juce::BigInteger&, bool enabled) const;
    bool hasPatchState(const juce::BigInteger&, bool enabled) const;
    bool waitForPatch(const juce::BigInteger&, bool enabled) const;

    juce::DynamicLibrary library;
    long (*logout)() = nullptr;
    long (*refresh)() = nullptr;
    long (*readParameter)(const char*, float*) = nullptr;
    long (*writeParameters)(const char*) = nullptr;
    juce::BigInteger selected;
    juce::BigInteger enabledByHost;
    bool loggedIn = false;
};
