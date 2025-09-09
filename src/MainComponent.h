#pragma once

#include <JuceHeader.h>
#include "dsp/OnsetDetector.h"
#include "dsp/TempoEstimator.h"
#include "dsp/BeatTracker.h"
#include <array>
 #include <deque>
#include <thread>
#include <mutex>
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

    // Constructor helpers (implementation split into separate translation units)
    void setupLabelsAndStatus();
    void setupLoopbackUI();
    void setupMidiUI();
    void setupPrefilterControls();
    void setupOSC();
    void startTimersAndThreads();
    
    // Loopback helpers
    bool startLoopbackCaptureForEndpoint (const juce::String& outputName);
    void handleLoopbackSamples (const float* interleaved, int frames, int chans, double sr, double qpcSeconds);

    // UI
    juce::Label statusLabel;
    juce::Label bpmLabel;
    juce::Label beatLabel;
    juce::Label confLabel;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector; // not shown when using loopback
    juce::ComboBox loopbackBox;
    juce::TextButton refreshLoopbackButton { "Refresh loopback" };
    juce::TextButton applyLoopbackButton { "Use loopback" };
    juce::TextButton changeLoopbackButton { "Change loopback" };
    juce::Label loopbackHint;

    // Prefilter controls
    juce::Label hpfHint;
    juce::Slider hpfSlider;
    juce::Label lpfHint;
    juce::Slider lpfSlider;

    // Debug toggle for OSC candidate peaks
    juce::ToggleButton showCandToggle { "Send cand. OSC" };

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

    // Multiresolution multiband onset detection: 20-150, 150-400, 400-800, 800-2000, 2000-6000 Hz
    // High- and low-resolution detectors per band
    std::array<std::unique_ptr<OnsetDetector>, 5> bandOnsetsHi;
    std::array<std::unique_ptr<OnsetDetector>, 5> bandOnsetsLo;
    // Per-band bandpass filters (HPF + LPF per band) used to feed detectors with band-limited audio
    std::array<juce::dsp::ProcessorChain<juce::dsp::IIR::Filter<float>, juce::dsp::IIR::Filter<float>>, 5> perBandFilters;
    std::unique_ptr<TempoEstimator> tempoEstimator;
    std::unique_ptr<BeatTracker> beatTracker;
    std::atomic<uint64_t> capturedSamples { 0 };
    std::array<std::deque<double>, 5> recentBandOnsets;
    double bandOnsetWindowSec { 4.0 };
    std::mutex bandMutex; // guards access to bandOnsets during capture/timer/prepare

    // Pending flux buffers (high-resolution) to align frames across bands before combining
    std::array<std::vector<float>, 5> pendingFluxHi;

    // EWMA normalization state for per-band flux fusion
    std::array<float, 5> fusionEwmaMean { 0,0,0,0,0 };
    std::array<float, 5> fusionEwmaVar  { 0,0,0,0,0 };
    std::array<bool, 5>  fusionEwmaInit { false,false,false,false,false };

    // OSC streaming
    juce::OSCSender osc;
    bool oscConnected { false };
    std::array<float, 5> bandActivity { 0,0,0,0,0 };

    // MIDI streaming
    std::unique_ptr<juce::MidiOutput> midiOut;
    int midiCcForTempo { 20 }; // default CC number for tempo
    int midiChannel { 1 };
    int midiBeatNote { 60 }; // C4 for beat pulses

    bool usingLoopback { false };
    juce::String preferredOutputName { "Głośniki" }; // target output device friendly name (e.g., Speakers/Głośniki)
    bool sendTempoCandidates { false };
    double minConfidenceForUpdates { 0.2 };
    // Onset merge and coincidence gating params
    double coincidenceWindowSec { 0.015 }; // small fixed window for multi-band coincidence
    int minBandsForOnset { 2 };

    // DSP worker thread to decouple capture and processing
    std::thread dspThread;
    std::atomic<bool> dspRunning { false };
    void startDspThread();
    void stopDspThread();

    // Wall-clock mapping from WASAPI QPC to sample time
    std::atomic<double> lastQpcSeconds { 0.0 };

    void refreshLoopbackList();
    bool selectLoopbackByOutputName (const juce::String& nameKeyword);

#if JUCE_WINDOWS
    std::unique_ptr<WASAPILoopbackCapture> loopbackCapture;
#endif

    void prepareProcessing (double sr, int samplesPerBlockExpected);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};


