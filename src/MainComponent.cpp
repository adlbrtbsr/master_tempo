#include "MainComponent.h"
#include <algorithm>
#include <cmath>

MainComponent::MainComponent()
{
    addAndMakeVisible(statusLabel);
    addAndMakeVisible(bpmLabel);
    addAndMakeVisible(beatLabel);

    // Style labels for dark background
    for (auto* lbl : { &statusLabel, &bpmLabel, &beatLabel })
    {
        lbl->setColour (juce::Label::textColourId, juce::Colours::white);
        lbl->setInterceptsMouseClicks (false, false);
    }
    statusLabel.setFont (juce::Font (18.0f));
    bpmLabel.setFont (juce::Font (18.0f));
    beatLabel.setFont (juce::Font (18.0f));

    statusLabel.setText("Initializing...", juce::dontSendNotification);
    bpmLabel.setText("BPM: --", juce::dontSendNotification);
    beatLabel.setText("Beat: --", juce::dontSendNotification);

    // Configure input-only audio; prefer WASAPI loopback on Windows
    setAudioChannels (0, 0); // disable JUCE device IO, we'll use WASAPI loopback directly
   #if JUCE_WINDOWS
    loopbackCapture = std::make_unique<WASAPILoopbackCapture>();
    const bool started = loopbackCapture->start (preferredOutputName, [this](const float* interleaved, int frames, int chans, double sr)
    {
        // Downmix to mono and feed our pipeline
        juce::HeapBlock<float> mono (frames);
        for (int i = 0; i < frames; ++i)
        {
            double sum = 0.0;
            for (int c = 0; c < chans; ++c)
                sum += interleaved[i * chans + c];
            mono[i] = (float) (sum / juce::jmax (1, chans));
        }

        // Lazy prepare on first callback
        if (currentSampleRate.load() != sr)
            prepareProcessing (sr, 512);

        float* channels[1] = { mono.get() };
        juce::dsp::AudioBlock<float> block (channels, 1, (size_t) frames);
        juce::dsp::ProcessContextReplacing<float> ctx (block);
        bandFilter.process (ctx);
        for (auto& d : bandOnsets)
            if (d)
                d->pushAudio (mono.get(), frames);

        audioTimeSec += (double) frames / (double) currentSampleRate.load();

        int start1, size1, start2, size2;
        fifo.prepareToWrite (frames, start1, size1, start2, size2);
        if (size1 > 0) juce::FloatVectorOperations::copy (ringBuffer.getWritePointer(0) + start1, mono.get(), size1);
        if (size2 > 0) juce::FloatVectorOperations::copy (ringBuffer.getWritePointer(0) + start2, mono.get() + size1, size2);
        fifo.finishedWrite (size1 + size2);
    });
    usingLoopback = started;
   #endif

    deviceSelector.reset (new juce::AudioDeviceSelectorComponent (deviceManager,
                                                                  0, 0, // min/max inputs (disable mic)
                                                                  0, 0, // min/max outputs (disable)
                                                                  false, false, false, false));
    // hide selector entirely; only keep our loopback UI

    // Loopback quick selector UI
    addAndMakeVisible (loopbackHint);
    loopbackHint.setText ("System output (loopback):", juce::dontSendNotification);
    loopbackHint.setColour (juce::Label::textColourId, juce::Colours::silver);
    addAndMakeVisible (loopbackBox);
    addAndMakeVisible (refreshLoopbackButton);
    addAndMakeVisible (applyLoopbackButton);
    addAndMakeVisible (changeLoopbackButton);
    refreshLoopbackButton.onClick = [this]{ refreshLoopbackList(); };
    changeLoopbackButton.onClick = [this]
    {
        usingLoopback = false;
        loopbackHint.setVisible (true);
        loopbackBox.setVisible (true);
        refreshLoopbackButton.setVisible (true);
        applyLoopbackButton.setVisible (true);
        changeLoopbackButton.setVisible (false);
        resized();
    };
    applyLoopbackButton.onClick = [this]
    {
        const int idx = loopbackBox.getSelectedItemIndex();
        if (idx < 0) return;
       #if JUCE_WINDOWS
        // Restart capture with the selected render endpoint name
        juce::StringArray endpoints = WASAPILoopbackCapture::listRenderEndpoints();
        if (idx < endpoints.size())
        {
            const juce::String chosen = endpoints[idx];
            loopbackCapture.reset (new WASAPILoopbackCapture());
            const bool started = loopbackCapture->start (chosen, [this](const float* interleaved, int frames, int chans, double sr)
            {
                juce::HeapBlock<float> mono (frames);
                for (int i = 0; i < frames; ++i)
                {
                    double sum = 0.0;
                    for (int c = 0; c < chans; ++c) sum += interleaved[i * chans + c];
                    mono[i] = (float) (sum / juce::jmax (1, chans));
                }
                if (currentSampleRate.load() != sr) prepareProcessing (sr, 512);
                float* channels[1] = { mono.get() };
                juce::dsp::AudioBlock<float> block (channels, 1, (size_t) frames);
                juce::dsp::ProcessContextReplacing<float> ctx (block);
                bandFilter.process (ctx);
                for (auto& d : bandOnsets)
                    if (d) d->pushAudio (mono.get(), frames);
                audioTimeSec += (double) frames / (double) currentSampleRate.load();
                int start1, size1, start2, size2;
                fifo.prepareToWrite (frames, start1, size1, start2, size2);
                if (size1 > 0) juce::FloatVectorOperations::copy (ringBuffer.getWritePointer(0) + start1, mono.get(), size1);
                if (size2 > 0) juce::FloatVectorOperations::copy (ringBuffer.getWritePointer(0) + start2, mono.get() + size1, size2);
                fifo.finishedWrite (size1 + size2);
            });
            usingLoopback = started;
            if (started)
            {
                statusLabel.setText (juce::String ("Loopback started: ") + chosen,
                                     juce::dontSendNotification);
                // Hide loopback controls to free UI space
                loopbackHint.setVisible (false);
                loopbackBox.setVisible (false);
                refreshLoopbackButton.setVisible (false);
                applyLoopbackButton.setVisible (false);
                changeLoopbackButton.setVisible (true);
                resized();
            }
            else
            {
                juce::String reason;
                if (loopbackCapture)
                    reason = loopbackCapture->getLastError();
                statusLabel.setText (juce::String ("Failed to start loopback: ") + chosen +
                                     (reason.isNotEmpty() ? (" (" + reason + ")") : juce::String()),
                                     juce::dontSendNotification);
            }
        }
       #endif
    };
    refreshLoopbackList();

    // MIDI UI
    addAndMakeVisible (midiHint);
    midiHint.setText ("MIDI out:", juce::dontSendNotification);
    midiHint.setColour (juce::Label::textColourId, juce::Colours::silver);
    addAndMakeVisible (midiOutBox);
    addAndMakeVisible (refreshMidiButton);
    addAndMakeVisible (connectMidiButton);
    refreshMidiButton.onClick = [this]
    {
        midiOutBox.clear();
        auto devices = juce::MidiOutput::getAvailableDevices();
        int idx = 0;
        for (auto& d : devices)
            midiOutBox.addItem (d.name, ++idx);
        statusLabel.setText ("MIDI outs: " + juce::String (devices.size()), juce::dontSendNotification);
    };
    connectMidiButton.onClick = [this]
    {
        const int idx = midiOutBox.getSelectedItemIndex();
        auto devices = juce::MidiOutput::getAvailableDevices();
        if (idx >= 0 && idx < devices.size())
        {
            midiOut.reset();
            midiOut = juce::MidiOutput::openDevice (devices[(int) idx].identifier);
            if (midiOut)
                statusLabel.setText ("MIDI connected: " + devices[(int) idx].name, juce::dontSendNotification);
            else
                statusLabel.setText ("Failed to open MIDI: " + devices[(int) idx].name, juce::dontSendNotification);
        }
    };
    refreshMidiButton.onClick();

    // Connect OSC to localhost:9000
    oscConnected = osc.connect ("127.0.0.1", 9000);
    startTimerHz (15);

    // Set size after child components exist to avoid early resized() touching nulls
    setSize (900, 600);

    // Receive raw input via device callback for loopback
    // No JUCE audio callback; we pull from WASAPI loopback thread
}

MainComponent::~MainComponent()
{
    deviceManager.removeAudioCallback (this);
    shutdownAudio();
}

void MainComponent::prepareToPlay (int samplesPerBlockExpected, double sr)
{
    prepareProcessing (sr, samplesPerBlockExpected);
}

void MainComponent::getNextAudioBlock (const juce::AudioSourceChannelInfo& bufferToFill)
{
    // Output-only rendering not used; keep output silent
    if (bufferToFill.buffer != nullptr)
        bufferToFill.clearActiveBufferRegion();
}

void MainComponent::releaseResources()
{
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colours::black);
}

void MainComponent::resized()
{
    auto r = getLocalBounds().reduced (10);
    statusLabel.setBounds (r.removeFromTop (24));
    bpmLabel.setBounds (r.removeFromTop (24));
    beatLabel.setBounds (r.removeFromTop (24));

    // Loopback quick selector row (only reserve space if visible)
    if (loopbackHint.isVisible() || loopbackBox.isVisible() || refreshLoopbackButton.isVisible() || applyLoopbackButton.isVisible())
    {
        auto row = r.removeFromTop (28);
        loopbackHint.setBounds (row.removeFromLeft (170));
        loopbackBox.setBounds (row.removeFromLeft (jmax (200, row.getWidth() - 320)));
        refreshLoopbackButton.setBounds (row.removeFromLeft (120));
        applyLoopbackButton.setBounds (row.removeFromLeft (120));
    }

    // Change loopback button sits below when loopback is active
    if (changeLoopbackButton.isVisible())
    {
        auto row2 = r.removeFromTop (28);
        changeLoopbackButton.setBounds (row2.removeFromLeft (160));
    }

    // MIDI row
    {
        auto row3 = r.removeFromTop (28);
        midiHint.setBounds (row3.removeFromLeft (100));
        midiOutBox.setBounds (row3.removeFromLeft (jmax (200, row3.getWidth() - 240)));
        refreshMidiButton.setBounds (row3.removeFromLeft (120));
        connectMidiButton.setBounds (row3.removeFromLeft (120));
    }
    // hide device selector; no mic choice presented
}

void MainComponent::timerCallback()
{
    if (tempoEstimator && beatTracker)
    {
        // Gather flux from each band
        std::array<std::vector<float>, 4> bandFluxFrames;
        for (size_t i = 0; i < bandOnsets.size(); ++i)
            if (bandOnsets[i])
                bandOnsets[i]->fetchNewFlux(bandFluxFrames[i]);

        // Combine per frame after per-band normalization
        size_t maxFrames = 0;
        for (const auto& v : bandFluxFrames)
            maxFrames = juce::jmax(maxFrames, v.size());

        if (maxFrames > 0)
        {
            std::vector<float> combined(maxFrames, 0.0f);
            for (size_t b = 0; b < bandFluxFrames.size(); ++b)
            {
                auto& v = bandFluxFrames[b];
                if (v.empty()) continue;
                float mean = 0.0f;
                for (float x : v) mean += x;
                mean /= (float) v.size();
                float var = 0.0f;
                for (float x : v) { float d = x - mean; var += d * d; }
                var /= (float) juce::jmax(1u, (unsigned int) v.size());
                const float stddev = std::sqrt(var) + 1e-6f;
                for (size_t i = 0; i < v.size(); ++i)
                {
                    const float z = (v[i] - mean) / stddev;
                    combined[i] += z * 0.25f; // equal weight across 4 bands
                }
            }
            tempoEstimator->appendFlux(combined);
        }

        // Merge onsets from all bands
        std::vector<double> mergedOnsets;
        for (auto& det : bandOnsets)
        {
            if (!det) continue;
            std::vector<double> bandOn;
            det->fetchOnsets(bandOn);
            mergedOnsets.insert(mergedOnsets.end(), bandOn.begin(), bandOn.end());
        }
        if (!mergedOnsets.empty())
        {
            std::sort(mergedOnsets.begin(), mergedOnsets.end());
            mergedOnsets.erase(std::unique(mergedOnsets.begin(), mergedOnsets.end(), [](double a, double b){ return std::abs(a - b) < 0.02; }), mergedOnsets.end());
            beatTracker->onOnsets(mergedOnsets);
            if (oscConnected)
            {
                for (auto t : mergedOnsets)
                    osc.send ("/beat", (float) t);
            }
            // Send MIDI beat pulses
            if (midiOut)
            {
                juce::MidiBuffer buffer;
                const int vel = 100;
                for (size_t i = 0; i < mergedOnsets.size(); ++i)
                {
                    buffer.addEvent (juce::MidiMessage::noteOn  (midiChannel, midiBeatNote, (juce::uint8) vel), 0);
                    buffer.addEvent (juce::MidiMessage::noteOff (midiChannel, midiBeatNote), 60); // short gate
                }
                midiOut->sendBlockOfMessagesNow (buffer);
            }
        }

        auto normalizeBpm = [] (double b)
        {
            if (b <= 0.0) return b;
            while (b < 120.0) b *= 2.0;
            while (b > 240.0) b /= 2.0;
            return b;
        };
        const double rawBpm = tempoEstimator->getBpm();
        const double conf = tempoEstimator->getConfidence();
        const double bpm = normalizeBpm (rawBpm);

        // Update beat tracker with normalized BPM to avoid octave errors
        beatTracker->updateBpm(bpm);

        if (bpm > 0)
            bpmLabel.setText ("BPM: " + juce::String(bpm, 1) + " (" + juce::String(conf, 2) + ")", juce::dontSendNotification);
        else
            bpmLabel.setText ("BPM: --", juce::dontSendNotification);

        // Send normalized BPM over OSC
        if (oscConnected)
            osc.send ("/tempo", (float) bpm, (float) conf);

        // Stream MIDI CC for tempo mapped 120..240 -> 0..127
        if (midiOut)
        {
            const double norm = juce::jlimit (120.0, 240.0, bpm);
            const int value = juce::roundToInt ((norm - 120.0) * (127.0 / 120.0));
            const int scaled = juce::jlimit<int> (0, 127, value);
            midiOut->sendMessageNow (juce::MidiMessage::controllerEvent (midiChannel, midiCcForTempo, scaled));
        }

        const double nextBeat = beatTracker->getNextBeatTimeSec(audioTimeSec);
        if (nextBeat > 0)
            beatLabel.setText ("Next beat: " + juce::String(nextBeat, 2) + " s", juce::dontSendNotification);
        else
            beatLabel.setText ("Beat: --", juce::dontSendNotification);
    }
    repaint();
}

void MainComponent::audioDeviceAboutToStart (juce::AudioIODevice*) {}

void MainComponent::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                      int numInputChannels,
                                                      float* const* outputChannelData,
                                                      int numOutputChannels,
                                                      int numSamples,
                                                      const juce::AudioIODeviceCallbackContext&)
{
    // Silence any outputs
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (auto* out = outputChannelData[ch]) juce::FloatVectorOperations::clear (out, numSamples);
}
void MainComponent::prepareProcessing (double sr, int samplesPerBlockExpected)
{
    currentSampleRate = sr;
    blockSize = samplesPerBlockExpected;

    dspSpec.sampleRate = sr;
    dspSpec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlockExpected);
    dspSpec.numChannels = 1;

    bandFilter.reset();
    bandFilter.prepare(dspSpec);
    bandFilter.get<0>().coefficients = juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, 20.0f);
    bandFilter.get<1>().coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (sr, 2000.0f);

    // Multiband onset detectors
    bandOnsets[0] = std::make_unique<OnsetDetector>(static_cast<int>(sr), 1024, 256,  20.0f, 150.0f);
    bandOnsets[1] = std::make_unique<OnsetDetector>(static_cast<int>(sr), 1024, 256, 150.0f, 400.0f);
    bandOnsets[2] = std::make_unique<OnsetDetector>(static_cast<int>(sr), 1024, 256, 400.0f, 800.0f);
    bandOnsets[3] = std::make_unique<OnsetDetector>(static_cast<int>(sr), 1024, 256, 800.0f, 2000.0f);
    tempoEstimator = std::make_unique<TempoEstimator>(sr, 256);
    beatTracker = std::make_unique<BeatTracker>(sr);

    audioTimeSec = 0.0;
    totalBlocks.store(0);

    statusLabel.setText ("Audio ready (loopback): SR=" + juce::String(sr) + ", block=" + juce::String(samplesPerBlockExpected), juce::dontSendNotification);
}

void MainComponent::refreshLoopbackList()
{
	loopbackBox.clear();
   #if JUCE_WINDOWS
	// Prefer direct MMDevice render endpoints for reliability
	juce::StringArray endpoints = WASAPILoopbackCapture::listRenderEndpoints();
	int added = 0;
	int preferredIndex = -1;
	for (int i = 0; i < endpoints.size(); ++i)
	{
		const auto& name = endpoints.getReference(i);
		loopbackBox.addItem (name, ++added);
		if (preferredIndex < 0 && (name.containsIgnoreCase (preferredOutputName) || name.containsIgnoreCase ("Speakers")))
			preferredIndex = added - 1;
	}
	if (added > 0)
		loopbackBox.setSelectedItemIndex (preferredIndex >= 0 ? preferredIndex : 0, juce::dontSendNotification);
	statusLabel.setText (added > 0 ? ("Render endpoints: " + juce::String (added))
	                              : juce::String ("No render endpoints detected"),
	                    juce::dontSendNotification);
   #endif
}

bool MainComponent::selectLoopbackByOutputName (const juce::String& nameKeyword)
{
   #if JUCE_WINDOWS
    juce::AudioIODeviceType* wasapiType = nullptr;
    for (auto* t : deviceManager.getAvailableDeviceTypes())
        if (t && t->getTypeName() == "Windows Audio") { wasapiType = t; break; }
    if (wasapiType == nullptr)
        return false;
    wasapiType->scanForDevices();
    auto inputNames = wasapiType->getDeviceNames (true);

    // Look for a loopback whose base output name matches the keyword
    juce::String chosen;
    for (auto& name : inputNames)
    {
        if (name.containsIgnoreCase ("loopback") && name.containsIgnoreCase (nameKeyword))
        {
            chosen = name;
            break;
        }
    }
    if (chosen.isEmpty())
        return false;

    auto setup = deviceManager.getAudioDeviceSetup();
    setup.inputDeviceName = chosen;
    setup.outputDeviceName.clear();
    setup.useDefaultInputChannels = true;
    setup.useDefaultOutputChannels = false;
    setup.bufferSize = 512;
    deviceManager.setAudioDeviceSetup (setup, true);
    usingLoopback = (deviceManager.getCurrentAudioDevice() != nullptr
                     && deviceManager.getCurrentAudioDevice()->getName() == chosen);
    if (usingLoopback)
        statusLabel.setText ("Audio ready (loopback): SR=" + juce::String(currentSampleRate.load()) +
                             ", block=" + juce::String(blockSize.load()), juce::dontSendNotification);
    return usingLoopback;
   #else
    juce::ignoreUnused (nameKeyword);
    return false;
   #endif
}


