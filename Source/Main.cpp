#include "AudioEngine.h"
#include "PluginScan.h"
#include "VoicemeeterInsert.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>
#include <array>
#include <cmath>
#include <thread>
#include <utility>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace
{
const juce::Colour background(0xff171b1e);
const juce::Colour panel(0xff21272b);
const juce::Colour textColour(0xffe6ebec);
const juce::Colour secondary(0xffaebcc0);
const juce::Colour accent(0xff83d5bf);

class PeakMeter final : public juce::Component
{
public:
    void setLevel(float decibels)
    {
        const auto next = juce::jlimit(0.0f, 1.0f, (decibels + 90.0f) / 90.0f);
        if (next != level)
        {
            level = next;
            repaint();
        }
    }

    void paint(juce::Graphics& graphics) override
    {
        const auto bounds = getLocalBounds().toFloat();
        graphics.setColour(background);
        graphics.fillRoundedRectangle(bounds, 4.0f);
        graphics.setColour(accent);
        const auto width = bounds.getWidth() * level;
        if (width > 0.0f)
            graphics.fillRoundedRectangle(bounds.withWidth(width), juce::jmin(4.0f, width / 2.0f));
    }

private:
    float level = 0.0f;
};

class EditorWindow final : public juce::DocumentWindow
{
public:
    EditorWindow(juce::AudioPluginInstance& plugin, std::function<void()> onClose)
        : DocumentWindow(plugin.getName(), background, closeButton), closed(std::move(onClose))
    {
        setUsingNativeTitleBar(true);
        std::unique_ptr<juce::AudioProcessorEditor> editor(plugin.createEditorIfNeeded());
        if (editor == nullptr)
            editor = std::make_unique<juce::GenericAudioProcessorEditor>(plugin);
        setResizable(editor->isResizable(), false);
        setContentOwned(editor.release(), true);
        centreWithSize(getWidth(), getHeight());
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        setVisible(false);
        if (closed)
            closed();
    }

private:
    std::function<void()> closed;
};

class MainView final : public juce::Component,
                       private juce::ListBoxModel,
                       private juce::ChangeListener,
                       private juce::Timer
{
public:
    explicit MainView(juce::PropertiesFile& userSettings)
        : settings(userSettings), devicePicker(engine.devices, 1, 2, 1, 2, false, false, false, false),
          effectsList("Effect chain", this)
    {
        setupLabel(title, "Relay Host", 26.0f, textColour, true);
        setupLabel(streamInfo, "Opening audio devices", 14.0f, secondary);
        setupLabel(audioHeading, "Audio routing", 17.0f, textColour, true);
        setupLabel(chainHeading, "Effects", 17.0f, textColour, true);
        setupLabel(inputLabel, "Input", 13.0f, secondary);
        setupLabel(outputLabel, "Output", 13.0f, secondary);
        setupLabel(status, {}, 13.0f, secondary);
        status.setMinimumHorizontalScale(0.85f);

        deviceViewport.setViewedComponent(&devicePicker, false);
        deviceViewport.setScrollBarsShown(true, false);
        addAndMakeVisible(deviceViewport);
        addAndMakeVisible(effectsList);
        addAndMakeVisible(catalog);
        catalog.setTitle("Discovered plug-ins");
        catalog.setTextWhenNothingSelected("Discovered plug-ins");
        catalog.onChange = [this] { updateButtons(); };
        effectsList.setRowHeight(32);
        effectsList.setColour(juce::ListBox::backgroundColourId, background);

        addAndMakeVisible(inputMeter);
        addAndMakeVisible(outputMeter);

        setupButton(startButton, "Start audio", [this] { toggleAudio(); });
        setupButton(chainBypass, "Bypass chain", [this] { setChainBypass(chainBypass.getToggleState()); });
        chainBypass.setClickingTogglesState(true);
        setupButton(reconnect, "Reconnect", [this] { openSavedRoute(); });
        setupButton(loadPlugin, "Open plug-in...", [this] { chooseScanPath(false); });
        setupButton(scanFolder, "Scan folder...", [this]
        {
            if (scanner.isRunning())
                scanner.cancel();
            else
                chooseScanPath(true);
        });
        setupButton(addPlugin, "Add", [this]
        {
            const auto index = catalog.getSelectedItemIndex();
            if (juce::isPositiveAndBelow(index, static_cast<int>(discovered.size())))
                appendEffect(discovered[static_cast<size_t>(index)]);
        });
        setupButton(openEditor, "Edit", [this] { showEditor(effectsList.getSelectedRow()); });
        setupButton(toggleEffect, "Bypass", [this]
        {
            engine.toggleEffect(effectsList.getSelectedRow());
            updateChain();
        });
        setupButton(moveUp, "Up", [this] { moveSelected(-1); });
        setupButton(moveDown, "Down", [this] { moveSelected(1); });
        setupButton(removeEffect, "Remove", [this]
        {
            editor.reset();
            engine.removeEffect(effectsList.getSelectedRow());
            updateChain();
        });

        lastRoute = settings.getXmlValue("route");
        const auto error = engine.devices.initialise(2, 2, lastRoute.get(), lastRoute == nullptr);
        engine.devices.addAudioCallback(&engine);
        engine.devices.addChangeListener(this);
        if (error.isNotEmpty())
            setStatus(error);
        else
        {
            rememberRoute();
            setStatus("Select devices and channels, then start audio.");
        }

        restoreCatalog();
        if (auto chain = settings.getXmlValue("effects"))
        {
            const auto errors = engine.restoreChain(*chain);
            if (!errors.isEmpty())
                setStatus("Unavailable: " + errors[0]);
        }

        effectsList.updateContent();
        updateButtons();
        startTimerHz(12);
    }

    ~MainView() override
    {
        stopTimer();
        voicemeeterInsert.release();
        engine.running.store(false);
        engine.devices.removeChangeListener(this);
        editor.reset();
        save();
    }

    void save()
    {
        if (lastRoute != nullptr)
            settings.setValue("route", lastRoute.get());
        settings.setValue("effects", engine.saveChain().get());
        settings.saveIfNeeded();
    }

    void hideEditor()
    {
        if (editor != nullptr)
            editor->setVisible(false);
    }

    void paint(juce::Graphics& graphics) override
    {
        graphics.fillAll(background);
        graphics.setColour(panel);
        graphics.fillRoundedRectangle(audioPanel.toFloat(), 8.0f);
        graphics.fillRoundedRectangle(chainPanel.toFloat(), 8.0f);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(16);
        auto header = area.removeFromTop(54);
        auto actions = header.removeFromRight(258).reduced(0, 9);
        chainBypass.setBounds(actions.removeFromRight(124));
        actions.removeFromRight(8);
        startButton.setBounds(actions);
        title.setBounds(header.removeFromTop(31));
        streamInfo.setBounds(header);

        area.removeFromTop(10);
        status.setBounds(area.removeFromBottom(25));
        area.removeFromBottom(6);
        audioPanel = area.removeFromLeft(area.getWidth() * 49 / 100);
        area.removeFromLeft(12);
        chainPanel = area;

        auto audio = audioPanel.reduced(12);
        auto audioHeader = audio.removeFromTop(29);
        reconnect.setBounds(audioHeader.removeFromRight(90));
        audioHeading.setBounds(audioHeader);
        audio.removeFromTop(6);
        auto meters = audio.removeFromBottom(48);
        auto in = meters.removeFromTop(21);
        inputLabel.setBounds(in.removeFromLeft(123));
        inputMeter.setBounds(in.reduced(0, 4));
        auto out = meters.removeFromBottom(21);
        outputLabel.setBounds(out.removeFromLeft(123));
        outputMeter.setBounds(out.reduced(0, 4));
        audio.removeFromBottom(5);
        deviceViewport.setBounds(audio);
        devicePicker.setSize(juce::jmax(60, audio.getWidth() - deviceViewport.getScrollBarThickness()),
                             devicePicker.getHeight());

        auto chain = chainPanel.reduced(12);
        chainHeading.setBounds(chain.removeFromTop(29));
        chain.removeFromTop(6);
        auto discover = chain.removeFromTop(28);
        loadPlugin.setBounds(discover.removeFromLeft(discover.getWidth() / 2 - 4));
        discover.removeFromLeft(8);
        scanFolder.setBounds(discover);
        chain.removeFromTop(7);
        auto add = chain.removeFromTop(28);
        addPlugin.setBounds(add.removeFromRight(52));
        add.removeFromRight(7);
        catalog.setBounds(add);
        chain.removeFromTop(8);
        auto controls = chain.removeFromBottom(29);
        for (auto* button : { &openEditor, &toggleEffect, &moveUp, &moveDown })
        {
            button->setBounds(controls.removeFromLeft((chain.getWidth() - 24) / 5));
            controls.removeFromLeft(6);
        }
        removeEffect.setBounds(controls);
        chain.removeFromBottom(8);
        effectsList.setBounds(chain);
    }

private:
    void setupLabel(juce::Label& label, const juce::String& value, float size,
                    juce::Colour colour, bool bold = false)
    {
        label.setText(value, juce::dontSendNotification);
        label.setFont(juce::FontOptions(size, bold ? juce::Font::bold : juce::Font::plain));
        label.setColour(juce::Label::textColourId, colour);
        label.setBorderSize(juce::BorderSize<int>(0));
        addAndMakeVisible(label);
    }

    void setupButton(juce::TextButton& button, const juce::String& label,
                     std::function<void()> action)
    {
        button.setButtonText(label);
        button.onClick = std::move(action);
        addAndMakeVisible(button);
    }

    void setStatus(const juce::String& value)
    {
        status.setText(value, juce::dontSendNotification);
        status.setTooltip(value);
    }

    void rememberRoute()
    {
        if (engine.streamReady.load())
        {
            if (auto xml = engine.devices.createStateXml())
            {
                lastRoute = std::move(xml);
                settings.setValue("route", lastRoute.get());
            }
        }
    }

    void changeListenerCallback(juce::ChangeBroadcaster*) override
    {
        rememberRoute();
    }

public:
    bool isAudioRunning() const { return engine.running.load(); }
    bool isChainBypassed() const { return engine.bypass.load(); }

    void setChainBypass(bool bypassed)
    {
        engine.bypass.store(bypassed);
        chainBypass.setToggleState(bypassed, juce::dontSendNotification);
    }

    void openSavedRoute()
    {
        if (const auto result = voicemeeterInsert.release(); result.failed())
        {
            setStatus(result.getErrorMessage());
            return;
        }
        engine.devices.closeAudioDevice();
        const auto error = engine.devices.initialise(2, 2, lastRoute.get(), lastRoute == nullptr);
        setStatus(error.isEmpty() ? "Audio route reopened." : error);
    }

    void toggleAudio()
    {
        if (engine.running.load())
        {
            const auto result = voicemeeterInsert.release();
            engine.running.store(false);
            setStatus(result.wasOk() ? "Audio stopped." : result.getErrorMessage());
            updateStreamInfo();
            return;
        }

        auto* device = engine.devices.getCurrentAudioDevice();
        if (device == nullptr || !engine.streamReady.load())
        {
            setStatus("Select an enabled input channel and output channel first.");
            return;
        }

        engine.interrupted.store(false);
        engine.running.store(true);
        if (VoicemeeterInsert::matchesDriver(device->getTypeName(), device->getName()))
        {
            const auto result = voicemeeterInsert.engage(device->getActiveInputChannels(),
                                                        device->getActiveOutputChannels());
            if (result.failed() || !engine.running.load())
            {
                voicemeeterInsert.release();
                engine.running.store(false);
                setStatus(result.failed() ? result.getErrorMessage() : "The insert audio stream was interrupted.");
                updateStreamInfo();
                return;
            }
            setStatus("Voicemeeter confirmed the selected insert returns.");
        }
        else
            setStatus("Audio is live. Closing this window keeps the chain running in the tray.");
        updateStreamInfo();
    }

private:
    void updateStreamInfo()
    {
        const bool active = engine.running.load();
        const bool ready = engine.streamReady.load();
        startButton.setButtonText(active ? "Stop audio" : "Start audio");
        reconnect.setEnabled(!ready || !active);
        juce::String info = active ? (ready ? "Running" : "Waiting for audio route") : "Stopped";
        if (auto* device = engine.devices.getCurrentAudioDevice(); device != nullptr && ready)
            info << "  |  " << juce::String(device->getCurrentSampleRate() / 1000.0, 1) << " kHz"
                 << "  |  " << device->getCurrentBufferSizeSamples() << " frames"
                 << "  |  " << engine.inputCount.load() << " in / " << engine.outputCount.load() << " out";
        if (active && voicemeeterInsert.hasRoute())
            info << (voicemeeterInsert.returnsAreOn() ? "  |  Insert return active" : "  |  Insert return off");
        streamInfo.setText(info, juce::dontSendNotification);
    }

    void restoreCatalog()
    {
        if (auto xml = settings.getXmlValue("catalog"))
            for (auto* item : xml->getChildWithTagNameIterator("PLUGIN"))
            {
                juce::PluginDescription description;
                if (description.loadFromXml(*item))
                    discovered.push_back(std::move(description));
            }
        refreshCatalog();
    }

    void refreshCatalog()
    {
        catalog.clear(juce::dontSendNotification);
        juce::XmlElement cache("Catalog");
        int id = 1;
        for (const auto& item : discovered)
        {
            catalog.addItem(item.name + "  (" + item.pluginFormatName + ")", id++);
            cache.addChildElement(item.createXml().release());
        }
        settings.setValue("catalog", &cache);
        updateButtons();
    }

    void chooseScanPath(bool folder)
    {
        const auto standard = juce::File("C:/Program Files/Common Files/VST3");
        chooser = std::make_unique<juce::FileChooser>(
            folder ? "Choose a folder of effects" : "Choose a VST3 bundle or a VST2 DLL",
            browseLocation.isDirectory() ? browseLocation : standard,
            folder ? "*" : "*.vst3;*.dll", folder);
        const auto flags = juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories
                         | (folder ? 0 : juce::FileBrowserComponent::canSelectFiles);
        chooser->launchAsync(flags, [safe = juce::Component::SafePointer<MainView>(this), folder]
                             (const juce::FileChooser& finished)
        {
            if (safe == nullptr || finished.getResults().isEmpty())
                return;
            const auto path = finished.getResult();
            if (!folder && !PluginScan::isPluginPath(path))
            {
                safe->setStatus("Choose a .vst3 bundle or a 64-bit .dll effect.");
                return;
            }
            safe->browseLocation = folder ? path : path.getParentDirectory();
            safe->addSingleResult = !folder;
            safe->scanner.start(path);
            safe->setStatus("Scanning " + path.getFileName() + "...");
            safe->updateButtons();
        });
    }

    void showEditor(int index)
    {
        if (auto* effect = engine.getEffect(index); effect != nullptr && effect->instance != nullptr)
        {
            editor.reset();
            editor = std::make_unique<EditorWindow>(*effect->instance, [this] { save(); });
        }
    }

    void appendEffect(const juce::PluginDescription& description)
    {
        juce::String error;
        if (!engine.addEffect(description, error))
        {
            setStatus(description.name + ": " + error);
            return;
        }
        updateChain();
        effectsList.selectRow(engine.getNumEffects() - 1);
        setStatus("Added " + description.name + ".");
    }

    void moveSelected(int offset)
    {
        const auto from = effectsList.getSelectedRow();
        const auto to = from + offset;
        engine.moveEffect(from, to);
        updateChain();
        effectsList.selectRow(to);
    }

    void updateChain()
    {
        effectsList.updateContent();
        effectsList.repaint();
        updateButtons();
        save();
    }

    void updateButtons()
    {
        const auto selected = effectsList.getSelectedRow();
        const auto* effect = engine.getEffect(selected);
        openEditor.setEnabled(effect != nullptr && effect->instance != nullptr);
        toggleEffect.setEnabled(effect != nullptr && effect->instance != nullptr);
        toggleEffect.setButtonText(effect != nullptr && !effect->enabled ? "Enable" : "Bypass");
        moveUp.setEnabled(effect != nullptr && selected > 0);
        moveDown.setEnabled(effect != nullptr && selected + 1 < engine.getNumEffects());
        removeEffect.setEnabled(effect != nullptr);
        addPlugin.setEnabled(catalog.getSelectedItemIndex() >= 0);
        loadPlugin.setEnabled(!scanner.isRunning());
        scanFolder.setButtonText(scanner.isRunning() ? "Cancel scan" : "Scan folder...");
    }

    int getNumRows() override { return engine.getNumEffects(); }
    void selectedRowsChanged(int) override { updateButtons(); }
    void listBoxItemDoubleClicked(int index, const juce::MouseEvent&) override { showEditor(index); }

    void paintListBoxItem(int index, juce::Graphics& graphics, int width, int height, bool selected) override
    {
        if (const auto* effect = engine.getEffect(index))
        {
            if (selected)
                graphics.fillAll(juce::Colour(0xff344844));
            graphics.setColour(effect->enabled && effect->instance != nullptr ? textColour : secondary);
            const juce::String suffix = effect->instance == nullptr ? "   |   Unavailable"
                                      : !effect->enabled ? "   |   Bypassed" : "";
            graphics.drawFittedText(juce::String(index + 1) + "  " + effect->description.name + suffix,
                                    8, 0, width - 16, height, juce::Justification::centredLeft, 1);
        }
    }

    void timerCallback() override
    {
        const auto peaks = engine.getPeakLevels();
        const auto updateMeter = [](PeakMeter& meter, juce::Label& label, const juce::String& name, float gain)
        {
            const auto decibels = juce::Decibels::gainToDecibels(gain, -90.0f);
            meter.setLevel(decibels);
            label.setText(name + "  " + (decibels > -90.0f ? juce::String(decibels, 1) : "-inf")
                          + " dBFS", juce::dontSendNotification);
        };
        updateMeter(inputMeter, inputLabel, "Input", peaks.input);
        updateMeter(outputMeter, outputLabel, "Output", peaks.output);
        if (engine.interrupted.exchange(false))
        {
            const auto result = voicemeeterInsert.release();
            setStatus(result.wasOk() ? "Audio paused after a route change. Check the devices and press Start audio."
                                     : result.getErrorMessage());
        }
        if (auto scanned = scanner.takeResult())
        {
            for (const auto& description : scanned->effects)
                if (std::none_of(discovered.begin(), discovered.end(), [&] (const auto& known)
                                 { return known.isDuplicateOf(description); }))
                    discovered.push_back(description);

            refreshCatalog();
            if (addSingleResult && scanned->effects.size() == 1)
                appendEffect(scanned->effects.front());
            else
            {
                auto message = juce::String(scanned->effects.size()) + " effects found.";
                if (!scanned->failures.isEmpty())
                    message << " Skipped " << scanned->failures.size() << " ("
                            << scanned->failures[0] << ").";
                setStatus(message);
            }
        }
        updateStreamInfo();
    }

    juce::PropertiesFile& settings;
    AudioEngine engine;
    VoicemeeterInsert voicemeeterInsert;
    PluginScan scanner;
    juce::AudioDeviceSelectorComponent devicePicker;
    juce::Viewport deviceViewport;
    juce::ListBox effectsList;
    juce::ComboBox catalog;
    PeakMeter inputMeter, outputMeter;
    juce::Label title, streamInfo, audioHeading, chainHeading, inputLabel, outputLabel, status;
    juce::TextButton startButton, chainBypass, reconnect, loadPlugin, scanFolder, addPlugin,
                     openEditor, toggleEffect, moveUp, moveDown, removeEffect;
    juce::Rectangle<int> audioPanel, chainPanel;
    std::vector<juce::PluginDescription> discovered;
    std::unique_ptr<juce::XmlElement> lastRoute;
    std::unique_ptr<juce::FileChooser> chooser;
    juce::File browseLocation;
    bool addSingleResult = false;
    std::unique_ptr<EditorWindow> editor;
};

class HostWindow final : public juce::DocumentWindow
{
public:
    explicit HostWindow(juce::PropertiesFile& settings)
        : DocumentWindow("Relay Host", background, allButtons)
    {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainView(settings), false);
        setResizable(true, false);
        setResizeLimits(760, 510, 1500, 1000);
        centreWithSize(960, 580);
        setVisible(true);
    }

    void closeButtonPressed() override
    {
        if (auto* view = static_cast<MainView*>(getContentComponent()))
        {
            view->hideEditor();
            view->save();
        }
        setVisible(false);
    }

    void open()
    {
        setVisible(true);
        setMinimised(false);
        toFront(true);
    }

    MainView& view() const { return *static_cast<MainView*>(getContentComponent()); }
};

class TrayIcon final : public juce::SystemTrayIconComponent
{
public:
    explicit TrayIcon(HostWindow& owner) : window(owner)
    {
        juce::Image icon(juce::Image::ARGB, 32, 32, true);
        juce::Graphics graphics(icon);
        graphics.setColour(background);
        graphics.fillRoundedRectangle(0, 0, 32, 32, 7);
        graphics.setColour(accent);
        graphics.fillRoundedRectangle(6, 13, 4, 6, 2);
        graphics.fillRoundedRectangle(14, 7, 4, 18, 2);
        graphics.fillRoundedRectangle(22, 11, 4, 10, 2);
        setIconImage(icon, icon);
        setIconTooltip("Relay Host");
    }

    void mouseDown(const juce::MouseEvent& event) override
    {
        showOnRelease = event.mods.isRightButtonDown();
        if (!showOnRelease && event.mods.isLeftButtonDown())
            window.open();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (std::exchange(showOnRelease, false))
            showMenu();
    }

private:
    void showMenu()
    {
        const auto owner = static_cast<HWND>(getWindowHandle());
        POINT cursor;
        if (owner == nullptr || !GetCursorPos(&cursor))
            return;

        const auto menu = CreatePopupMenu();
        if (menu == nullptr)
            return;

        enum Command : UINT { open = 1, toggleAudio, bypass, reconnect, quit };
        auto& view = window.view();
        const auto running = view.isAudioRunning();
        AppendMenuW(menu, MF_STRING, open, L"Open Relay Host");
        AppendMenuW(menu, MF_STRING, toggleAudio, running ? L"Stop audio" : L"Start audio");
        AppendMenuW(menu, MF_STRING | (view.isChainBypassed() ? MF_CHECKED : MF_UNCHECKED),
                    bypass, L"Bypass effects");
        AppendMenuW(menu, MF_STRING | (running ? MF_GRAYED : MF_ENABLED), reconnect, L"Reconnect audio");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, quit, L"Quit Relay Host");
        SetMenuDefaultItem(menu, open, FALSE);

        SetForegroundWindow(owner);
        const auto action = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON,
                                             cursor.x, cursor.y, owner, nullptr);
        PostMessageW(owner, WM_NULL, 0, 0);
        DestroyMenu(menu);

        switch (action)
        {
            case open: window.open(); break;
            case toggleAudio:
                view.toggleAudio();
                if (!running && !view.isAudioRunning())
                    window.open();
                break;
            case bypass: view.setChainBypass(!view.isChainBypassed()); break;
            case reconnect: view.openSavedRoute(); window.open(); break;
            case quit: juce::JUCEApplication::getInstance()->systemRequestedQuit(); break;
            default: break;
        }
    }

    HostWindow& window;
    bool showOnRelease = false;
};

int runSelfTest(const juce::File& report)
{
    AudioEngine engine;
    const std::array<float, 4> input { 0.1f, -0.2f, 0.3f, -0.4f };
    std::array<float, 4> left {}, right {};
    const float* inputs[] { nullptr, input.data() };
    float* outputs[] { nullptr, left.data(), nullptr, right.data() };
    engine.running.store(true);
    engine.audioDeviceIOCallbackWithContext(inputs, 2, outputs, 4, 4, {});
    const auto routed = input == left && input == right && engine.getPeakLevels().output == 0.4f;

    const std::array<float, 4> louder { 0.2f, -0.4f, 0.6f, -0.8f };
    std::atomic<bool> done { false };
    std::thread writer([&]
    {
        for (int block = 0; block < 50'000; ++block)
        {
            const float* channels[] { (block % 2 == 0 ? input : louder).data() };
            engine.audioDeviceIOCallbackWithContext(channels, 1, outputs, 4, 4, {});
        }
        done.store(true);
    });
    bool metersAgree = true;
    while (!done.load())
    {
        const auto peaks = engine.getPeakLevels();
        if (peaks.input != peaks.output)
        {
            metersAgree = false;
            break;
        }
    }
    writer.join();

    const float* stereoInputs[] { input.data(), louder.data() };
    engine.audioDeviceIOCallbackWithContext(stereoInputs, 2, outputs, 4, 4, {});
    const auto stereoPeaks = engine.getPeakLevels();
    const bool stereoIsDry = left == input && right == louder
                          && stereoPeaks.input == 0.8f && stereoPeaks.output == 0.8f;

    engine.running.store(false);
    engine.audioDeviceIOCallbackWithContext(inputs, 2, outputs, 4, 4, {});
    const bool stopped = left == std::array<float, 4> {} && right == left;
    const auto passed = routed && stereoIsDry && stopped && metersAgree;
    const auto message = passed ? "PASS bit-exact mono/stereo routing, paired dry meters, channel gaps, and stopped output"
                         : !metersAgree ? "FAIL empty chain shows mismatched input/output peaks"
                                        : "FAIL audio routing or stopped output";
    return report.replaceWithText(message) && passed ? 0 : 1;
}

int listDevices(const juce::File& report)
{
    juce::AudioDeviceManager devices;
    juce::String lines;
    for (auto* type : devices.getAvailableDeviceTypes())
    {
        type->scanForDevices();
        lines << type->getTypeName() << "\n";
        for (const auto& name : type->getDeviceNames(true))
            lines << "  IN  " << name << "\n";
        for (const auto& name : type->getDeviceNames(false))
            lines << "  OUT " << name << "\n";
    }
    return report.replaceWithText(lines) && lines.isNotEmpty() ? 0 : 1;
}

int checkDevice(const juce::StringArray& args)
{
    AudioEngine engine;
    juce::String detail("Backend or endpoint unavailable");
    for (auto* type : engine.devices.getAvailableDeviceTypes())
    {
        if (type->getTypeName() != args[1])
            continue;
        type->scanForDevices();
        std::unique_ptr<juce::AudioIODevice> device(type->createDevice(args[3], args[2]));
        if (device == nullptr)
            break;
        juce::BigInteger inputs, outputs;
        inputs.setRange(args[4].getIntValue(), 2, true);
        outputs.setRange(args[5].getIntValue(), 2, true);
        detail = device->open(inputs, outputs, 0, 0);
        if (detail.isNotEmpty())
            break;
        device->start(&engine);
        juce::Thread::sleep(400);
        const auto ready = engine.streamReady.load() && device->isPlaying() && engine.processedBlocks.load() > 0;
        detail = juce::String(ready ? "PASS " : "FAIL ") + device->getName()
               + " at " + juce::String(device->getCurrentSampleRate()) + " Hz, "
               + juce::String(engine.inputCount.load()) + " in / " + juce::String(engine.outputCount.load())
               + " out, " + juce::String(engine.processedBlocks.load()) + " callbacks"
               + ". Input channels: " + device->getInputChannelNames().joinIntoString(", ")
               + ". Output channels: " + device->getOutputChannelNames().joinIntoString(", ");
        device->stop();
        device->close();
        break;
    }
    return juce::File(args[6]).replaceWithText(detail) && detail.startsWith("PASS ") ? 0 : 1;
}

int checkPlugin(const juce::File& path, const juce::File& report)
{
    PluginScan scanner;
    scanner.start(path);
    while (scanner.isRunning())
        juce::Thread::sleep(25);
    auto scanned = scanner.takeResult();
    if (!scanned || scanned->effects.empty())
    {
        report.replaceWithText("FAIL scanning " + path.getFileName());
        return 1;
    }

    AudioEngine engine;
    juce::String error;
    for (const auto& description : scanned->effects)
    {
        if (!engine.addEffect(description, error))
            continue;

        std::array<float, 512> input {}, left {}, right {};
        const float* inputs[] { input.data() };
        float* outputs[] { left.data(), right.data() };
        engine.running.store(true);
        for (int block = 0; block < 16; ++block)
        {
            for (size_t i = 0; i < input.size(); ++i)
                input[i] = std::sin((block * input.size() + i) * 0.0576f) * 0.25f;
            engine.audioDeviceIOCallbackWithContext(inputs, 1, outputs, 2, 512, {});
        }
        const auto valid = std::all_of(left.begin(), left.end(), [] (float sample) { return std::isfinite(sample); })
                        && std::all_of(right.begin(), right.end(), [] (float sample) { return std::isfinite(sample); });
        const auto wetPeak = engine.getPeakLevels().output;
        auto state = engine.saveChain();
        const auto restored = state->getFirstChildElement() != nullptr;

        const auto returnsDry = [&]
        {
            engine.audioDeviceIOCallbackWithContext(inputs, 1, outputs, 2, 512, {});
            const auto peaks = engine.getPeakLevels();
            return input == left && input == right && peaks.input == peaks.output;
        };
        engine.bypass.store(true);
        const auto chainBypassed = returnsDry();
        engine.bypass.store(false);
        engine.toggleEffect(0);
        const auto effectBypassed = returnsDry();
        engine.toggleEffect(0);
        engine.removeEffect(0);
        const auto effectRemoved = returnsDry() && engine.getNumEffects() == 0
                                && engine.saveChain()->getFirstChildElement() == nullptr;
        const auto passed = valid && restored && chainBypassed && effectBypassed && effectRemoved;
        const auto line = juce::String(passed ? "PASS " : "FAIL ") + description.pluginFormatName
                        + " " + description.name + " processed 16 buffers; wet peak "
                        + juce::String(wetPeak, 4)
                        + (passed ? "; chain bypass, effect bypass, and removal return bit-exact dry audio."
                                  : "; dry audio or state check failed.");
        return report.replaceWithText(line) && passed ? 0 : 1;
    }
    report.replaceWithText("FAIL loading effect: " + error);
    return 1;
}

class RelayApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Relay Host"; }
    const juce::String getApplicationVersion() override { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override { return true; }

    void initialise(const juce::String&) override
    {
        const auto args = getCommandLineParameterArray();
        if (args.size() == 3 && args[0] == "--probe")
            return finish(PluginScan::probe(juce::File(args[1]), juce::File(args[2])));
        if (args.size() == 2 && args[0] == "--self-test")
            return finish(runSelfTest(juce::File(args[1])));
        if (args.size() == 2 && args[0] == "--list-devices")
            return finish(listDevices(juce::File(args[1])));
        if (args.size() == 7 && args[0] == "--check-device")
            return finish(checkDevice(args));
        if (args.size() == 3 && args[0] == "--check-plugin")
            return finish(checkPlugin(juce::File(args[1]), juce::File(args[2])));

        if (!instanceLock.enter(0))
            return finish(1);
        ownsLock = true;

        juce::PropertiesFile::Options options;
        options.applicationName = "Relay Host";
        options.filenameSuffix = "settings";
        options.folderName = "Relay Host";
        properties.setStorageParameters(options);

        look.setColour(juce::ResizableWindow::backgroundColourId, background);
        look.setColour(juce::TextButton::buttonColourId, juce::Colour(0xff364146));
        look.setColour(juce::TextButton::buttonOnColourId, juce::Colour(0xff305a50));
        look.setColour(juce::ComboBox::backgroundColourId, background);
        look.setColour(juce::ComboBox::textColourId, textColour);
        juce::LookAndFeel::setDefaultLookAndFeel(&look);
        window = std::make_unique<HostWindow>(*properties.getUserSettings());
        tray = std::make_unique<TrayIcon>(*window);
    }

    void shutdown() override
    {
        tray.reset();
        window.reset();
        properties.closeFiles();
        juce::LookAndFeel::setDefaultLookAndFeel(nullptr);
        if (ownsLock)
            instanceLock.exit();
    }

private:
    void finish(int result)
    {
        setApplicationReturnValue(result);
        quit();
    }

    juce::InterProcessLock instanceLock { "RelayHost-Desktop-0.1" };
    bool ownsLock = false;
    juce::ApplicationProperties properties;
    juce::LookAndFeel_V4 look;
    std::unique_ptr<HostWindow> window;
    std::unique_ptr<TrayIcon> tray;
};
}

START_JUCE_APPLICATION(RelayApplication)
