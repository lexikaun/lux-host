# Relay Host

Relay Host is a 64-bit Windows tray app for routing a live input through one mono or stereo chain of VST2 and VST3 effects. It uses the audio drivers already installed on the computer.

## Run the demo

1. Open `build/RelayHost_artefacts/Release/Relay Host.exe`.
2. In **Audio routing**, select a driver type, device or devices, and the desired input and output channels. Scroll the audio panel to see all controls for the selected driver.
3. Click **Start audio**. The input and output meters show peak dBFS from the same audio block. **Bypass chain** passes dry audio at unity gain. An empty chain also passes dry audio at unity gain. Stereo-to-mono routing averages the two channels.
4. Use **Open plug-in** for an individual VST2 DLL or VST3 file or bundle. **Scan folder** adds discovered effects to the picker. Select one and click **Add**. Double-click a chain entry to open its editor.
5. Close the window to keep audio running in the Windows notification area. Left-click the Relay Host icon to reopen it. Right-click for **Open Relay Host**, **Start audio** or **Stop audio**, **Bypass effects**, **Reconnect audio**, and **Quit Relay Host**. If the icon is in the overflow area, open **Show hidden icons** first.

### Voicemeeter Banana

Select **ASIO** and **Voicemeeter Insert Virtual ASIO**. Select the same input and output channels for your strip. The first hardware strip uses **IN#1 Left** and **IN#1 Right**. **Start audio** enables the matching Banana **Patch Insert** returns and checks that Banana accepted them. Look for **Insert return active** in Relay Host. **Stop audio** releases only the returns Relay Host enabled. Monitor the same strip through its Banana output bus. The Insert driver accepts one host at a time, so close the ASIO route in LightHost or another client before opening it here. [VB-Audio Insert guide](https://voicemeeter.com/user-guide-connect-audio-apps-to-the-voicemeeter-insert-driver/)

Banana's strip fader applies its own gain after the insert return. Relay Host's output meter measures the signal before that fader. [VB-Audio signal flow](https://voicemeeter.com/user-guide-connect-audio-apps-to-the-voicemeeter-insert-driver/)

For a separate USB mic and virtual cable, choose **Windows Audio**, select the microphone as input and **CABLE Input** as output, and choose **CABLE Output** as the microphone in the receiving app. On this machine the standard cable's playback endpoint appears as **Speaker/Headphone (VB-Audio Virtual Cable)**. [VB-CABLE](https://vb-audio.com/Cable/)

Relay Host starts with audio stopped and pauses if its audio stream is interrupted. Use **Reconnect** to reopen the last saved route and **Start audio** to resume. Stopping audio sends silence and retains the current device connection. Use **Bypass chain** for dry audio.

## Build and verify

Install CMake 3.24 or later and Visual Studio 2022 Build Tools with Desktop development with C++. From PowerShell, run these commands.

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The build fetches pinned copies of JUCE and the Xaymar VST2 headers with a JUCE compatibility shim. The demo accepts existing x64 effect plug-ins. Plug-in discovery runs in a helper process. Loaded effects and their editors run in the host. [JUCE](https://github.com/juce-framework/JUCE) · [Xaymar VST2 headers](https://github.com/Xaymar/vst2sdk) · [JUCE compatibility shim](https://github.com/heide-oficial/Light-Host-Modern/tree/12cef28abb48a6df98a37de8254f3efd7e919db7/ThirdParty/XaymarVST2JuceShim)
