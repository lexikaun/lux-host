#include "VoicemeeterInsert.h"

namespace
{
juce::String patchName(int channel)
{
    return "Patch.insert[" + juce::String(channel) + "]";
}

juce::File remoteLibraryFile()
{
    using Registry = juce::WindowsRegistry;
    constexpr auto uninstallKey = R"(HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\VB:Voicemeeter {17359A74-1236-5467}\UninstallString)";
    auto uninstall = Registry::getValue(uninstallKey, {}, Registry::WoW64_32bit);
    if (uninstall.isEmpty())
        uninstall = Registry::getValue(uninstallKey, {}, Registry::WoW64_64bit);

    const auto lastSlash = uninstall.lastIndexOfChar('\\');
    if (lastSlash < 0)
        return {};

    const auto folder = uninstall.substring(0, lastSlash).trimCharactersAtStart("\"");
    if (!juce::File::isAbsolutePath(folder))
        return {};

    return juce::File(folder).getChildFile("VoicemeeterRemote64.dll");
}
}

VoicemeeterInsert::~VoicemeeterInsert()
{
    release();
    if (loggedIn)
        logout();
}

bool VoicemeeterInsert::matchesDriver(const juce::String& type, const juce::String& name)
{
    return type == "ASIO" && name.equalsIgnoreCase("Voicemeeter Insert Virtual ASIO");
}

juce::Result VoicemeeterInsert::connect()
{
    if (loggedIn)
        return juce::Result::ok();

    const auto dll = remoteLibraryFile();
    if (!dll.existsAsFile() || !library.open(dll.getFullPathName()))
        return juce::Result::fail("Voicemeeter Remote API is unavailable. Check the Voicemeeter installation.");

    const auto login = reinterpret_cast<long (*)()>(library.getFunction("VBVMR_Login"));
    logout = reinterpret_cast<decltype(logout)>(library.getFunction("VBVMR_Logout"));
    refresh = reinterpret_cast<decltype(refresh)>(library.getFunction("VBVMR_IsParametersDirty"));
    readParameter = reinterpret_cast<decltype(readParameter)>(library.getFunction("VBVMR_GetParameterFloat"));
    writeParameters = reinterpret_cast<decltype(writeParameters)>(library.getFunction("VBVMR_SetParameters"));
    if (!login || !logout || !refresh || !readParameter || !writeParameters)
        return juce::Result::fail("Voicemeeter Remote API is missing the insert controls.");

    const auto result = login();
    if (result != 0)
    {
        if (result >= 0)
            logout();
        return juce::Result::fail("Start Voicemeeter before opening its insert returns.");
    }

    loggedIn = true;
    return juce::Result::ok();
}

juce::Result VoicemeeterInsert::engage(const juce::BigInteger& inputs, const juce::BigInteger& outputs)
{
    if (inputs != outputs || inputs.isZero() || inputs.countNumberOfSetBits() > 2)
        return juce::Result::fail("Select the same Voicemeeter Insert channels for input and output.");

    if (const auto cleanup = release(); cleanup.failed())
        return cleanup;
    if (const auto connection = connect(); connection.failed())
        return connection;

    if (refresh() < 0)
        return juce::Result::fail("Cannot read Voicemeeter's insert settings.");

    juce::BigInteger enable;
    for (int channel = inputs.findNextSetBit(0); channel >= 0; channel = inputs.findNextSetBit(channel + 1))
    {
        float value = 0.0f;
        if (readParameter(patchName(channel).toRawUTF8(), &value) != 0)
            return juce::Result::fail("Voicemeeter cannot read the selected insert return.");
        if (value < 0.5f)
            enable.setBit(channel);
    }

    if (!sendPatch(enable, true))
        return juce::Result::fail("Voicemeeter rejected the insert return change.");

    enabledByHost = enable;
    if (!waitForPatch(inputs, true))
    {
        release();
        return juce::Result::fail("Voicemeeter did not confirm the selected insert returns.");
    }

    selected = inputs;
    return juce::Result::ok();
}

juce::Result VoicemeeterInsert::release()
{
    selected.clear();
    if (!sendPatch(enabledByHost, false) || !waitForPatch(enabledByHost, false))
        return juce::Result::fail("Could not restore the insert returns enabled by Relay Host.");

    enabledByHost.clear();
    return juce::Result::ok();
}

bool VoicemeeterInsert::returnsAreOn() const
{
    return hasRoute() && hasPatchState(selected, true);
}

bool VoicemeeterInsert::sendPatch(const juce::BigInteger& channels, bool enabled) const
{
    if (channels.isZero())
        return true;

    juce::String script;
    for (int channel = channels.findNextSetBit(0); channel >= 0; channel = channels.findNextSetBit(channel + 1))
        script << patchName(channel) << (enabled ? "=1;" : "=0;");
    return writeParameters(script.toRawUTF8()) == 0;
}

bool VoicemeeterInsert::hasPatchState(const juce::BigInteger& channels, bool enabled) const
{
    if (channels.isZero())
        return true;
    if (refresh() < 0)
        return false;

    for (int channel = channels.findNextSetBit(0); channel >= 0; channel = channels.findNextSetBit(channel + 1))
    {
        float value = 0.0f;
        if (readParameter(patchName(channel).toRawUTF8(), &value) != 0 || (value >= 0.5f) != enabled)
            return false;
    }
    return true;
}

bool VoicemeeterInsert::waitForPatch(const juce::BigInteger& channels, bool enabled) const
{
    const auto deadline = juce::Time::getMillisecondCounterHiRes() + 300.0;
    do
    {
        if (hasPatchState(channels, enabled))
            return true;
        juce::Thread::sleep(10);
    }
    while (juce::Time::getMillisecondCounterHiRes() < deadline);

    return false;
}
