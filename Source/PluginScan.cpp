#include "PluginScan.h"
#include <chrono>

namespace
{
void collectCandidates(const juce::File& path, juce::Array<juce::File>& candidates,
                       std::stop_token stop)
{
    if (stop.stop_requested())
        return;

    if (PluginScan::isPluginPath(path))
    {
        candidates.add(path);
        return;
    }

    if (path.isDirectory())
        for (const auto& entry : juce::RangedDirectoryIterator(path, false, "*",
                                                              juce::File::findFilesAndDirectories,
                                                              juce::File::FollowSymlinks::no))
            collectCandidates(entry.getFile(), candidates, stop);
}
}

bool PluginScan::isPluginPath(const juce::File& file)
{
    return (file.exists() && file.hasFileExtension("vst3"))
        || (file.existsAsFile() && file.hasFileExtension("dll"));
}

int PluginScan::probe(const juce::File& file, const juce::File& resultFile)
{
    if (!isPluginPath(file))
        return 1;

    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager(formats);
    juce::OwnedArray<juce::PluginDescription> found;
    const auto formatName = file.hasFileExtension("vst3") ? "VST3" : "VST";
    for (auto* format : formats.getFormats())
        if (format->getName() == formatName)
            format->findAllTypesForFile(found, file.getFullPathName());

    juce::XmlElement xml("Scan");
    for (auto* description : found)
        xml.addChildElement(description->createXml().release());
    return xml.writeTo(resultFile) && !found.isEmpty() ? 0 : 1;
}

void PluginScan::start(juce::File path)
{
    if (running.exchange(true))
        return;

    worker = std::jthread([this, path = std::move(path)] (std::stop_token stop)
    {
        Result scanned;
        juce::Array<juce::File> candidates;
        collectCandidates(path, candidates, stop);
        const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
        for (const auto& file : candidates)
        {
            if (stop.stop_requested())
                break;

            juce::TemporaryFile xmlFile(".xml");
            const auto report = xmlFile.getFile();
            juce::ChildProcess process;
            const bool launched = process.start({ executable.getFullPathName(), "--probe",
                                                   file.getFullPathName(), report.getFullPathName() }, 0);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
            while (launched && process.isRunning() && !stop.stop_requested()
                   && std::chrono::steady_clock::now() < deadline)
                process.waitForProcessToFinish(50);

            if (launched && process.isRunning())
            {
                process.kill();
                process.waitForProcessToFinish(200);
            }

            if (stop.stop_requested())
                break;

            auto xml = juce::parseXML(report);
            if (xml == nullptr || process.getExitCode() != 0)
            {
                scanned.failures.add(file.getFileName());
                continue;
            }

            for (auto* item : xml->getChildWithTagNameIterator("PLUGIN"))
            {
                juce::PluginDescription description;
                if (description.loadFromXml(*item) && !description.isInstrument)
                    scanned.effects.push_back(std::move(description));
            }
        }

        if (candidates.isEmpty() && !stop.stop_requested())
            scanned.failures.add("No VST2 or VST3 files found");
        {
            const std::scoped_lock lock(mutex);
            result = std::move(scanned);
        }
        running.store(false);
    });
}

void PluginScan::cancel()
{
    worker.request_stop();
}

std::optional<PluginScan::Result> PluginScan::takeResult()
{
    const std::scoped_lock lock(mutex);
    return std::exchange(result, std::nullopt);
}
