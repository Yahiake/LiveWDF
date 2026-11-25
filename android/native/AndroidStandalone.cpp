#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include "PluginEditor.h"
#include "PluginProcessor.h"

#if LIVEWDF_ANDROID_USB
#include <UsbAudioDevice.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <vector>

namespace {

bool callActivity(const char* method) {
    auto activity = juce::getMainActivity();
    if (activity == nullptr)
        return false;
    auto* env = juce::getEnv();
    juce::LocalRef<jclass> activityClass(env->GetObjectClass(activity.get()));
    const auto id = env->GetMethodID(activityClass.get(), method, "()V");
    if (id != nullptr)
        env->CallVoidMethod(activity.get(), id);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    return id != nullptr;
}

class StandalonePlayer final : public juce::Component, private juce::Timer {
public:
    StandalonePlayer() {
        juce::MemoryBlock saved;
        if (sessionFile().loadFileAsData(saved))
            processor.setStateInformation(saved.getData(), static_cast<int>(saved.getSize()));

        editor.reset(processor.createEditorIfNeeded());
        if (editor != nullptr)
            addAndMakeVisible(*editor);

        status.setText("Audio stopped. Connect headphones before monitoring.", juce::dontSendNotification);
        status.setColour(juce::Label::textColourId, juce::Colour(0xffb8c6d6));
        addAndMakeVisible(status);

        androidAudio.onClick = [this] { requestAndroidAudio(); };
        addAndMakeVisible(androidAudio);
        stop.onClick = [this] { stopAudio(); };
        addAndMakeVisible(stop);

#if LIVEWDF_ANDROID_USB
        usbAudio.onClick = [this] { requestAudio(true); };
        addAndMakeVisible(usbAudio);
        usbBlockSize.addItem("USB 128 samples", 128);
        usbBlockSize.addItem("USB 64 samples", 64);
        usbBlockSize.setSelectedId(128, juce::dontSendNotification);
        usbBlockSize.setTitle("USB audio buffer");
        addAndMakeVisible(usbBlockSize);
        usbDiagnostics.setColour(juce::Label::textColourId, juce::Colour(0xffb8c6d6));
        usbDiagnostics.setFont(juce::FontOptions(13.0f));
        addAndMakeVisible(usbDiagnostics);
        testUsbOutput.onClick = [this] { testToneRequested.store(true, std::memory_order_relaxed); };
        addChildComponent(testUsbOutput);
        startTimerHz(4);
#endif
    }

    ~StandalonePlayer() override {
        stopAudio(false);
        saveSession();
    }

    void resized() override {
        auto area = getLocalBounds();
        auto controls = area.removeFromTop(45).reduced(5, 3);
        androidAudio.setBounds(controls.removeFromLeft(controls.getWidth() / buttonCount).reduced(3, 0));
        if (usbAudio.isVisible())
            usbAudio.setBounds(controls.removeFromLeft(controls.getWidth() / 2).reduced(3, 0));
        stop.setBounds(controls.reduced(3, 0));
#if LIVEWDF_ANDROID_USB
        auto statusRow = area.removeFromTop(testUsbOutput.isVisible() ? 37 : 24);
        if (testUsbOutput.isVisible())
            testUsbOutput.setBounds(statusRow.removeFromRight(97).reduced(4, 2));
        auto diagnosticsRow = area.removeFromTop(37).reduced(5, 2);
        usbBlockSize.setBounds(diagnosticsRow.removeFromLeft(132));
        usbDiagnostics.setBounds(diagnosticsRow.reduced(4, 0));
#else
        auto statusRow = area.removeFromTop(24);
#endif
        status.setBounds(statusRow.reduced(7, 0));
        if (editor != nullptr)
            editor->setBounds(area);
    }

    void paint(juce::Graphics& graphics) override { graphics.fillAll(juce::Colour(0xff161d27)); }

    void stopAudio(bool releaseJavaConnection = true) {
        ++permissionRequest;
#if LIVEWDF_ANDROID_USB
        closeDirectUsb();
#endif
        deviceManager.removeAudioCallback(&player);
        deviceManager.closeAudioDevice();
        player.setProcessor(nullptr);
        if (releaseJavaConnection)
            callActivity("releaseUsbConnection");
        showStatus("Audio stopped");
    }

    void saveSession() {
        juce::MemoryBlock saved;
        processor.getStateInformation(saved);
        if (!saved.isEmpty())
            sessionFile().replaceWithData(saved.getData(), saved.getSize());
    }

    void showStatus(const juce::String& message) {
        status.setText(message, juce::dontSendNotification);
    }

#if LIVEWDF_ANDROID_USB
    juce::String startDirectUsb(int fd, const std::vector<std::uint8_t>& descriptors) {
        stopAudio(false);
        if (!directUsb.open(fd, descriptors))
            return usbFailure();

        const auto format = directUsb.format();
        const auto blockFrames = usbBlockSize.getSelectedId();
        inputChannels = format.inputChannels;
        outputChannels = format.outputChannels;
        testToneSampleRate = format.sampleRate;
        testTonePosition = format.sampleRate / 4;
        const int channels = std::min(2u, outputChannels);
        if (channels == 0) {
            directUsb.close();
            return "The USB interface has no playback channels";
        }

        const auto layout = juce::AudioChannelSet::canonicalChannelSet(channels);
        juce::AudioProcessor::BusesLayout buses;
        buses.inputBuses.add(layout);
        buses.outputBuses.add(layout);
        if (!processor.setBusesLayout(buses)) {
            directUsb.close();
            return "The processor could not use this USB channel layout";
        }

        usbBuffer.setSize(channels, blockFrames);
        usbMidi.ensureSize(256);
        processor.setRateAndBufferSizeDetails(format.sampleRate, blockFrames);
        processor.prepareToPlay(format.sampleRate, blockFrames);
        longestDspMicros.store(0, std::memory_order_relaxed);
        usbDiagnostics.setText({}, juce::dontSendNotification);
        usbPrepared = true;
        if (!directUsb.start(blockFrames, directUsbCallback, this)) {
            const auto message = usbFailure();
            closeDirectUsb();
            return message;
        }

        showStatus("USB direct · " + juce::String(format.sampleRate) + " Hz · "
                   + juce::String(inputChannels) + " in / " + juce::String(outputChannels) + " out · "
                   + juce::String(blockFrames) + " samples");
        usbBlockSize.setEnabled(false);
        testUsbOutput.setVisible(inputChannels == 0);
        resized();
        return {};
    }
#endif

private:
    static juce::File sessionFile() {
        return juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
            .getChildFile("LiveWDF-Standalone.state");
    }

    void requestAndroidAudio() { requestAudio(false); }

    void requestAudio(bool direct) {
        stopAudio();
        showStatus(direct ? "Opening USB audio…" : "Opening Android audio…");
        const auto request = ++permissionRequest;
        juce::RuntimePermissions::request(juce::RuntimePermissions::recordAudio,
            [safe = juce::Component::SafePointer<StandalonePlayer>(this), request, direct](bool granted) {
                if (safe == nullptr || request != safe->permissionRequest)
                    return;
                if (!granted) {
                    safe->showStatus("Microphone permission is needed to monitor audio");
                    return;
                }
                if (!direct) {
                    safe->startAndroidAudio();
                    return;
                }
                safe->showStatus("Select a USB audio interface");
                if (!callActivity("selectUsbAudio"))
                    safe->showStatus("Could not open the USB device picker");
            });
    }

    void startAndroidAudio() {
        const auto error = deviceManager.initialise(1, 2, nullptr, true);
        auto* device = deviceManager.getCurrentAudioDevice();
        if (error.isNotEmpty() || device == nullptr) {
            showStatus(error.isNotEmpty() ? error : "No Android audio device available");
            return;
        }
        player.setProcessor(&processor);
        deviceManager.addAudioCallback(&player);
        showStatus(device->getTypeName() + " · " + juce::String(device->getCurrentSampleRate(), 0)
                   + " Hz · " + juce::String(device->getCurrentBufferSizeSamples()) + " samples");
    }

    void timerCallback() override {
#if LIVEWDF_ANDROID_USB
        if (!usbPrepared)
            return;
        if (!directUsb.isRunning()) {
            const auto error = usbFailure();
            stopAudio();
            showStatus(error);
        } else {
            refreshUsbDiagnostics();
        }
#endif
    }

#if LIVEWDF_ANDROID_USB
    juce::String usbFailure() const {
        const auto message = directUsb.lastError();
        return message.empty() ? juce::String{"USB audio stopped"} : juce::String{message};
    }

    void refreshUsbDiagnostics() {
        const auto counters = directUsb.statistics();
        const auto peak = longestDspMicros.load(std::memory_order_relaxed);
        const auto text = "Recoveries " + juce::String(counters.captureRecoveries)
            + " · USB errors " + juce::String(counters.packetErrors)
            + " · DSP peak " + juce::String(peak / 1000.0, 2) + " ms";
        usbDiagnostics.setText(text, juce::dontSendNotification);
    }

    void closeDirectUsb() {
        directUsb.close();
        usbBlockSize.setEnabled(true);
        testToneRequested.store(false, std::memory_order_relaxed);
        if (testUsbOutput.isVisible()) {
            testUsbOutput.setVisible(false);
            resized();
        }
        if (usbPrepared) {
            refreshUsbDiagnostics();
            processor.releaseResources();
            usbPrepared = false;
        }
    }

    static void directUsbCallback(void* context, const float* input, float* output, unsigned frames) noexcept {
        auto& app = *static_cast<StandalonePlayer*>(context);
        auto& buffer = app.usbBuffer;
        const auto channels = static_cast<unsigned>(buffer.getNumChannels());
        std::fill_n(output, frames * app.outputChannels, 0.0f);
        if (frames != static_cast<unsigned>(buffer.getNumSamples()))
            return;

        for (unsigned channel = 0; channel < channels; ++channel) {
            auto* target = buffer.getWritePointer(static_cast<int>(channel));
            if (input == nullptr || app.inputChannels == 0) {
                std::fill_n(target, frames, 0.0f);
                continue;
            }
            const unsigned source = std::min(channel, app.inputChannels - 1);
            for (unsigned frame = 0; frame < frames; ++frame)
                target[frame] = input[frame * app.inputChannels + source];
        }

        app.usbMidi.clear();
        const auto began = std::chrono::steady_clock::now();
        app.processor.processBlock(buffer, app.usbMidi);
        const auto elapsed = static_cast<unsigned>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - began).count());
        if (elapsed > app.longestDspMicros.load(std::memory_order_relaxed))
            app.longestDspMicros.store(elapsed, std::memory_order_relaxed);
        for (unsigned channel = 0; channel < channels; ++channel) {
            const auto* source = buffer.getReadPointer(static_cast<int>(channel));
            for (unsigned frame = 0; frame < frames; ++frame)
                output[frame * app.outputChannels + channel] = source[frame];
        }
        if (app.inputChannels == 0)
            app.renderUsbTestTone(output, frames, channels);
    }

    void renderUsbTestTone(float* output, unsigned frames, unsigned channels) noexcept {
        if (testToneRequested.exchange(false, std::memory_order_relaxed))
            testTonePosition = 0;

        const auto length = testToneSampleRate / 4;
        if (testTonePosition >= length)
            return;
        const auto fadeLength = testToneSampleRate / 200;
        constexpr auto amplitude = 0.03162278f;
        const auto radiansPerSample = juce::MathConstants<double>::twoPi * 440.0 / testToneSampleRate;
        for (unsigned frame = 0; frame < frames && testTonePosition < length; ++frame, ++testTonePosition) {
            const auto edge = std::min(testTonePosition, length - 1 - testTonePosition);
            const auto envelope = static_cast<float>(std::min(edge, fadeLength)) / static_cast<float>(fadeLength);
            const auto value = amplitude * envelope
                             * static_cast<float>(std::sin(testTonePosition * radiansPerSample));
            for (unsigned channel = 0; channel < channels; ++channel)
                output[frame * outputChannels + channel] = value;
        }
    }

    static constexpr int buttonCount = 3;
    wdf::android::usb::Device directUsb;
    juce::AudioBuffer<float> usbBuffer;
    juce::MidiBuffer usbMidi;
    unsigned inputChannels = 0;
    unsigned outputChannels = 0;
    unsigned testToneSampleRate = 48000;
    unsigned testTonePosition = 48000 / 4;
    std::atomic<bool> testToneRequested{false};
    std::atomic<unsigned> longestDspMicros{0};
    juce::ComboBox usbBlockSize;
    juce::Label usbDiagnostics;
    juce::TextButton testUsbOutput{"Test output"};
    bool usbPrepared = false;
#else
    static constexpr int buttonCount = 2;
#endif

    livewdf::LiveWDFPluginProcessor processor;
    juce::AudioProcessorPlayer player;
    juce::AudioDeviceManager deviceManager;
    std::unique_ptr<juce::AudioProcessorEditor> editor;
    juce::TextButton androidAudio{"Android audio"};
    juce::TextButton usbAudio{"USB direct"};
    juce::TextButton stop{"Stop audio"};
    juce::Label status;
    unsigned permissionRequest = 0;
};

class LiveWdfApplication final : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "LiveWDF"; }
    const juce::String getApplicationVersion() override { return LIVEWDF_VERSION_STRING; }

    void initialise(const juce::String&) override {
        window = std::make_unique<juce::DocumentWindow>(getApplicationName(), juce::Colours::black, 0);
        player = new StandalonePlayer();
        window->setUsingNativeTitleBar(true);
        window->setFullScreen(true);
        window->setContentOwned(player, false);
        window->setVisible(true);
    }

    void shutdown() override {
        player = nullptr;
        window.reset();
    }

    static StandalonePlayer* activePlayer() {
        auto* app = static_cast<LiveWdfApplication*>(getInstance());
        return app != nullptr ? app->player : nullptr;
    }

private:
    std::unique_ptr<juce::DocumentWindow> window;
    StandalonePlayer* player = nullptr;
};

}  // namespace

START_JUCE_APPLICATION(LiveWdfApplication)

extern "C" JNIEXPORT void JNICALL
Java_com_yahia_livewdf_LiveWdfActivity_nativeStop(JNIEnv*, jobject) {
    if (auto* player = LiveWdfApplication::activePlayer()) {
        player->stopAudio(false);
        player->saveSession();
    }
}

extern "C" JNIEXPORT void JNICALL
Java_com_yahia_livewdf_LiveWdfActivity_nativeUsbStatus(JNIEnv* env, jobject, jstring message) {
    if (auto* player = LiveWdfApplication::activePlayer())
        player->showStatus(juce::juceString(env, message));
}

extern "C" JNIEXPORT jstring JNICALL
Java_com_yahia_livewdf_LiveWdfActivity_nativeStartUsb(JNIEnv* env, jobject, jint fd, jbyteArray descriptors) {
    juce::String error = "The direct USB backend is unavailable";
#if LIVEWDF_ANDROID_USB
    if (auto* player = LiveWdfApplication::activePlayer(); player != nullptr && descriptors != nullptr) {
        const auto size = env->GetArrayLength(descriptors);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
        if (size > 0)
            env->GetByteArrayRegion(descriptors, 0, size, reinterpret_cast<jbyte*>(bytes.data()));
        error = player->startDirectUsb(fd, bytes);
    }
#else
    juce::ignoreUnused(fd, descriptors);
#endif
    return error.isEmpty() ? nullptr : env->NewStringUTF(error.toRawUTF8());
}
