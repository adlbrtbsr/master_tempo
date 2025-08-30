#pragma once

#include <JuceHeader.h>
#include "dsp/OnsetDetector.h"
#include "dsp/TempoEstimator.h"
#include "dsp/BeatTracker.h"
#include <array>
#if JUCE_WINDOWS
#include "win/WASAPILoopback.h"
#endif

class MainComponent : public juce::AudioAppComponent,
                      public juce::AudioIODeviceCallback,
                      private juce::Timer
{
public:
    MainComponent();
    ~MainComponent() override;

    void prepareToPlay (int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    // AudioIODeviceCallback (input capture)
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice* device) override;
    void audioDeviceStopped() override {}

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;

    // UI
    juce::Label statusLabel;
    juce::Label bpmLabel;
    juce::Label beatLabel;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector; // not shown when using loopback
    juce::ComboBox loopbackBox;
    juce::TextButton refreshLoopbackButton { "Refresh loopback" };
    juce::TextButton applyLoopbackButton { "Use loopback" };
    juce::TextButton changeLoopbackButton { "Change loopback" };
    juce::Label loopbackHint;

    // MIDI out
    juce::Label midiHint;
    juce::ComboBox midiOutBox;
    juce::TextButton refreshMidiButton { "Refresh MIDI" };
    juce::TextButton connectMidiButton { "Connect MIDI" };

    // Audio processing state
    std::atomic<double> currentSampleRate { 0.0 };
    std::atomic<int> blockSize { 0 };

    // Ring buffer for callback->DSP handoff (mono)
    juce::AbstractFifo fifo { 1 << 14 }; // 16384 samples
    juce::AudioBuffer<float> ringBuffer { 1, 1 << 14 };

    // Band limiting: 100 Hz HPF -> 4 kHz LPF
    juce::dsp::ProcessorChain<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Filter<float>> bandFilter;
    juce::dsp::ProcessSpec dspSpec {};

    // Debug: counters
    std::atomic<uint64_t> totalBlocks { 0 };

    // Multiband onset detection: 20-150, 150-400, 400-800, 800-2000 Hz
    std::array<std::unique_ptr<OnsetDetector>, 4> bandOnsets;
    std::unique_ptr<TempoEstimator> tempoEstimator;
    std::unique_ptr<BeatTracker> beatTracker;
    double audioTimeSec { 0.0 };

    // OSC streaming
    juce::OSCSender osc;
    bool oscConnected { false };

    // MIDI streaming
    std::unique_ptr<juce::MidiOutput> midiOut;
    int midiCcForTempo { 20 }; // default CC number for tempo
    int midiChannel { 1 };
    int midiBeatNote { 60 }; // C4 for beat pulses

    bool usingLoopback { false };
    juce::String preferredOutputName { "Głośniki" }; // target output device friendly name (e.g., Speakers/Głośniki)

    void refreshLoopbackList();
    bool selectLoopbackByOutputName (const juce::String& nameKeyword);

#if JUCE_WINDOWS
    std::unique_ptr<WASAPILoopbackCapture> loopbackCapture;
#endif

    void prepareProcessing (double sr, int samplesPerBlockExpected);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};


