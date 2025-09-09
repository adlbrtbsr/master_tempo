#include "MainComponent.h"

void MainComponent::timerCallback()
{
    if (tempoEstimator && beatTracker)
    {
        std::array<std::vector<float>, 5> bandFluxFrames;
        {
            std::lock_guard<std::mutex> lock(bandMutex);
            for (size_t i = 0; i < bandOnsetsHi.size(); ++i)
                if (bandOnsetsHi[i])
                    bandOnsetsHi[i]->fetchNewFlux(bandFluxFrames[i]);
        }

        size_t minAvail = SIZE_MAX;
        for (size_t b = 0; b < bandFluxFrames.size(); ++b)
        {
            if (!bandFluxFrames[b].empty())
                pendingFluxHi[b].insert(pendingFluxHi[b].end(), bandFluxFrames[b].begin(), bandFluxFrames[b].end());
            minAvail = juce::jmin(minAvail, pendingFluxHi[b].size());
        }
        if (minAvail != SIZE_MAX && minAvail > 0)
        {
            std::vector<float> combined(minAvail, 0.0f);
            double weights[5] { 1,1,1,1,1 };
            double totalW = 0.0;
            for (size_t b = 0; b < 5; ++b)
            {
                const double wnd = juce::jmax(0.5, bandOnsetWindowSec);
                const double rate = recentBandOnsets[b].size() / wnd;
                const double w = 0.5 + 0.5 * (1.0 - std::exp(-rate));
                weights[b] = w;
                totalW += w;
            }
            if (totalW <= 1.0e-6) totalW = 1.0;

            for (size_t b = 0; b < 5; ++b)
            {
                auto& v = pendingFluxHi[b];
                if (v.size() < minAvail) continue;
                const float wnorm = (float) (weights[b] / totalW);
                float mean = fusionEwmaMean[b];
                float var  = fusionEwmaVar[b];
                bool  init = fusionEwmaInit[b];
                const float gamma = 0.03f;
                for (size_t i = 0; i < minAvail; ++i)
                {
                    const float x = v[i];
                    if (!init)
                    {
                        mean = x;
                        var  = 0.0f;
                        init = true;
                    }
                    else
                    {
                        const float dm = x - mean;
                        mean += gamma * dm;
                        var = (1.0f - gamma) * (var + gamma * dm * dm);
                    }
                    const float stddev = std::sqrt(juce::jmax(var, 1.0e-6f));
                    const float z = (x - mean) / stddev;
                    combined[i] += z * wnorm;
                }
                fusionEwmaMean[b] = mean;
                fusionEwmaVar[b]  = var;
                fusionEwmaInit[b] = init;
            }
            for (size_t b = 0; b < 5; ++b)
            {
                auto& v = pendingFluxHi[b];
                if (v.size() >= minAvail)
                    v.erase(v.begin(), v.begin() + (long) minAvail);
            }
            tempoEstimator->appendFlux(combined);
        }

        std::array<std::vector<double>, 5> cachedBandOnsets;
        std::vector<double> mergedOnsets;
        {
            std::lock_guard<std::mutex> lock(bandMutex);
            for (size_t i = 0; i < bandOnsetsHi.size(); ++i)
            {
                if (bandOnsetsHi[i])
                {
                    bandOnsetsHi[i]->fetchOnsets(cachedBandOnsets[i]);
                    if (!cachedBandOnsets[i].empty())
                        std::sort(cachedBandOnsets[i].begin(), cachedBandOnsets[i].end());
                    mergedOnsets.insert(mergedOnsets.end(), cachedBandOnsets[i].begin(), cachedBandOnsets[i].end());
                }
                if (bandOnsetsLo[i])
                {
                    std::vector<double> tmp;
                    bandOnsetsLo[i]->fetchOnsets(tmp);
                    if (!tmp.empty())
                        std::sort(tmp.begin(), tmp.end());
                    mergedOnsets.insert(mergedOnsets.end(), tmp.begin(), tmp.end());
                }
            }
        }
        if (!mergedOnsets.empty())
        {
            std::sort(mergedOnsets.begin(), mergedOnsets.end());
            const double fixedWin = juce::jlimit(0.008, 0.030, coincidenceWindowSec);
            std::vector<double> stage1;
            for (size_t i = 0; i < mergedOnsets.size(); )
            {
                double t0 = mergedOnsets[i];
                double sum = 0.0; int count = 0;
                size_t j = i;
                while (j < mergedOnsets.size() && (mergedOnsets[j] - t0) <= fixedWin)
                {
                    sum += mergedOnsets[j];
                    ++count; ++j;
                }
                stage1.push_back(sum / juce::jmax(1, count));
                i = j;
            }
            const double currentBpm = tempoEstimator ? tempoEstimator->getBpm() : -1.0;
            double per = currentBpm > 0.0 ? (60.0 / currentBpm) : 0.5;
            const double mergeWindow = juce::jlimit(0.01, 0.06, 0.10 * per);
            std::vector<double> stage2;
            for (double t : stage1)
            {
                if (stage2.empty() || std::abs(t - stage2.back()) > mergeWindow)
                    stage2.push_back(t);
            }
            mergedOnsets.swap(stage2);

            std::vector<double> gated;
            double weights[5] { 1,1,1,1,1 };
            double totalW = 0.0;
            for (size_t b = 0; b < 5; ++b)
            {
                const double wnd = juce::jmax(0.5, bandOnsetWindowSec);
                const double rate = recentBandOnsets[b].size() / wnd;
                const double w = 0.5 + 0.5 * (1.0 - std::exp(-rate));
                weights[b] = w; totalW += w;
            }
            if (totalW <= 1.0e-6) totalW = 1.0;
            for (double t : mergedOnsets)
            {
                int bands = 0;
                double wsum = 0.0;
                for (const auto& bo : cachedBandOnsets)
                {
                    auto it = std::lower_bound(bo.begin(), bo.end(), t - fixedWin);
                    if (it != bo.end() && std::abs(*it - t) <= fixedWin)
                    {
                        ++bands;
                        const size_t idx = (size_t) (&bo - &cachedBandOnsets[0]);
                        if (idx < 5)
                            wsum += weights[idx];
                    }
                }
                const double normalizedSupport = wsum / totalW;
                if (bands >= juce::jmax(1, minBandsForOnset) || normalizedSupport >= 0.6)
                    gated.push_back(t);
            }
            mergedOnsets.swap(gated);

            tempoEstimator->ingestOnsets(mergedOnsets);
            beatTracker->onOnsets(mergedOnsets);
            if (!mergedOnsets.empty())
            {
                const double latest = mergedOnsets.back();
                for (size_t b = 0; b < cachedBandOnsets.size(); ++b)
                {
                    auto& bo = cachedBandOnsets[b];
                    for (double t : bo)
                    {
                        auto& q = recentBandOnsets[b];
                        q.push_back(t);
                        while (!q.empty() && (latest - q.front()) > bandOnsetWindowSec) q.pop_front();
                    }
                }
            }
            if (oscConnected)
            {
                for (auto t : mergedOnsets)
                    osc.send ("/beat", (float) t);
            }
            if (midiOut)
            {
                juce::MidiBuffer buffer;
                const int vel = 100;
                for (size_t i = 0; i < mergedOnsets.size(); ++i)
                {
                    buffer.addEvent (juce::MidiMessage::noteOn  (midiChannel, midiBeatNote, (juce::uint8) vel), 0);
                    buffer.addEvent (juce::MidiMessage::noteOff (midiChannel, midiBeatNote), 60);
                }
                midiOut->sendBlockOfMessagesNow (buffer);
            }
        }

        const double bpm = tempoEstimator->getBpm();
        const double conf = tempoEstimator->getConfidence();

        static int stableTicks = 0;
        static double lastAppliedBpm = -1.0;
        if (conf >= juce::jmax(0.25, minConfidenceForUpdates))
        {
            const double rel = (lastAppliedBpm > 0.0 && bpm > 0.0) ? std::abs(bpm - lastAppliedBpm) / juce::jmax(1.0, lastAppliedBpm) : 0.0;
            if (rel < 0.04)
                ++stableTicks;
            else
                stableTicks = 0;

            if (stableTicks >= 3 && bpm > 0.0)
            {
                beatTracker->updateBpm(bpm);
                const double period = 60.0 / bpm;
                const double refr = juce::jlimit(0.04, 0.18, 0.20 * period);
                {
                    std::lock_guard<std::mutex> lock(bandMutex);
                    for (size_t b = 0; b < 5; ++b)
                    {
                        if (bandOnsetsHi[b]) bandOnsetsHi[b]->setRefractorySeconds(refr);
                        if (bandOnsetsLo[b]) bandOnsetsLo[b]->setRefractorySeconds(refr);
                    }
                }
                lastAppliedBpm = bpm;
                stableTicks = 0;
            }
        }

        if (bpm > 0)
            bpmLabel.setText ("BPM: " + juce::String(bpm, 1), juce::dontSendNotification);
        else
            bpmLabel.setText ("BPM: --", juce::dontSendNotification);
        confLabel.setText ("Conf: " + juce::String(conf, 2), juce::dontSendNotification);

        if (oscConnected)
            osc.send ("/tempo", (float) bpm, (float) conf);

        if (midiOut)
        {
            const double norm = juce::jlimit (60.0, 240.0, bpm);
            const int value = juce::roundToInt ((norm - 60.0) * (127.0 / 180.0));
            const int scaled = juce::jlimit<int> (0, 127, value);
            midiOut->sendMessageNow (juce::MidiMessage::controllerEvent (midiChannel, midiCcForTempo, scaled));
        }

        const double timeSecNow = (double) capturedSamples.load(std::memory_order_relaxed) / juce::jmax(1.0, currentSampleRate.load());
        const double nextBeat = beatTracker->getNextBeatTimeSec(timeSecNow);
        if (nextBeat > 0)
            beatLabel.setText ("Next beat: " + juce::String(nextBeat, 2) + " s", juce::dontSendNotification);
        else
            beatLabel.setText ("Beat: --", juce::dontSendNotification);
    }
    repaint();
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
    bandFilter.get<1>().coefficients = juce::dsp::IIR::Coefficients<float>::makeLowPass (sr, 6000.0f);
    {
        std::lock_guard<std::mutex> lock(bandMutex);
        auto makeHP = [sr](float fc){ return juce::dsp::IIR::Coefficients<float>::makeHighPass (sr, fc); };
        auto makeLP = [sr](float fc){ return juce::dsp::IIR::Coefficients<float>::makeLowPass  (sr, fc); };
        const float lows [5] = {  20.0f, 150.0f, 400.0f,  800.0f, 2000.0f };
        const float highs[5] = { 150.0f, 400.0f, 800.0f, 2000.0f, 6000.0f };
        for (size_t b = 0; b < 5; ++b)
        {
            perBandFilters[b].reset();
            perBandFilters[b].prepare(dspSpec);
            perBandFilters[b].get<0>().coefficients = makeHP(lows[b]);
            perBandFilters[b].get<1>().coefficients = makeLP(highs[b]);
        }
    }

    {
        std::lock_guard<std::mutex> lock(bandMutex);
        const int hopHi = juce::jmax(64, (int) juce::roundToInt(sr * 0.005));
        const int hopLo = juce::jmax(128, (int) juce::roundToInt(sr * 0.010));
        const int fftHi = 1024;
        const int fftLo = 2048;
        bandOnsetsHi[0] = std::make_unique<OnsetDetector>(static_cast<int>(sr), fftHi, hopHi,  20.0f, 150.0f);
        bandOnsetsHi[1] = std::make_unique<OnsetDetector>(static_cast<int>(sr), fftHi, hopHi, 150.0f, 400.0f);
        bandOnsetsHi[2] = std::make_unique<OnsetDetector>(static_cast<int>(sr), fftHi, hopHi, 400.0f, 800.0f);
        bandOnsetsHi[3] = std::make_unique<OnsetDetector>(static_cast<int>(sr), fftHi, hopHi, 800.0f, 2000.0f);
        bandOnsetsHi[4] = std::make_unique<OnsetDetector>(static_cast<int>(sr), fftHi, hopHi, 2000.0f, 6000.0f);
        bandOnsetsLo[0] = std::make_unique<OnsetDetector>(static_cast<int>(sr), fftLo, hopLo,  20.0f, 150.0f);
        bandOnsetsLo[1] = std::make_unique<OnsetDetector>(static_cast<int>(sr), fftLo, hopLo, 150.0f, 400.0f);
        bandOnsetsLo[2] = std::make_unique<OnsetDetector>(static_cast<int>(sr), fftLo, hopLo, 400.0f, 800.0f);
        bandOnsetsLo[3] = std::make_unique<OnsetDetector>(static_cast<int>(sr), fftLo, hopLo, 800.0f, 2000.0f);
        bandOnsetsLo[4] = std::make_unique<OnsetDetector>(static_cast<int>(sr), fftLo, hopLo, 2000.0f, 6000.0f);
        for (auto& d : bandOnsetsHi) if (d) d->setThresholdWindowSeconds(0.75);
        for (auto& d : bandOnsetsLo) if (d) d->setThresholdWindowSeconds(0.75);
        tempoEstimator = std::make_unique<TempoEstimator>(sr, hopHi);
    }
    if (!tempoEstimator)
        tempoEstimator = std::make_unique<TempoEstimator>(sr, 256);
    beatTracker = std::make_unique<BeatTracker>(sr);

    capturedSamples.store(0, std::memory_order_relaxed);
    totalBlocks.store(0);

    statusLabel.setText ("Audio ready (loopback): SR=" + juce::String(sr) + ", block=" + juce::String(samplesPerBlockExpected), juce::dontSendNotification);
}

void MainComponent::startDspThread()
{
    if (dspRunning.exchange(true)) return;
    dspThread = std::thread([this]
    {
        juce::HeapBlock<float> processBlock;
        while (dspRunning.load())
        {
            const int chunk = 512;
            if (currentSampleRate.load() <= 0.0)
            {
                juce::Thread::sleep (2);
                continue;
            }
            int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
            fifo.prepareToRead (chunk, start1, size1, start2, size2);
            const int total = size1 + size2;
            if (total <= 0)
            {
                juce::Thread::sleep (2);
                continue;
            }
            processBlock.allocate ((size_t) total, false);
            if (size1 > 0)
                juce::FloatVectorOperations::copy (processBlock.get(), ringBuffer.getReadPointer(0) + start1, size1);
            if (size2 > 0)
                juce::FloatVectorOperations::copy (processBlock.get() + size1, ringBuffer.getReadPointer(0) + start2, size2);
            fifo.finishedRead (total);

            float* channelsArr[1] = { processBlock.get() };
            juce::dsp::AudioBlock<float> blk (channelsArr, 1, (size_t) total);
            juce::dsp::ProcessContextReplacing<float> ctx (blk);
            bandFilter.process (ctx);
            {
                std::lock_guard<std::mutex> lock(bandMutex);
                juce::HeapBlock<float> bandBuf(total);
                for (size_t b = 0; b < 5; ++b)
                {
                    juce::FloatVectorOperations::copy (bandBuf.get(), processBlock.get(), total);
                    float* ch[1] = { bandBuf.get() };
                    juce::dsp::AudioBlock<float> bblk (ch, 1, (size_t) total);
                    juce::dsp::ProcessContextReplacing<float> bctx (bblk);
                    perBandFilters[b].process (bctx);
                    if (bandOnsetsHi[b]) bandOnsetsHi[b]->pushAudio (bandBuf.get(), total);
                    if (bandOnsetsLo[b]) bandOnsetsLo[b]->pushAudio (bandBuf.get(), total);
                }
            }
        }
    });
}

void MainComponent::stopDspThread()
{
    dspRunning.store(false);
    if (dspThread.joinable()) dspThread.join();
}

void MainComponent::audioDeviceAboutToStart (juce::AudioIODevice*) {}

void MainComponent::audioDeviceIOCallbackWithContext (const float* const* /*inputChannelData*/,
                                                      int /*numInputChannels*/,
                                                      float* const* outputChannelData,
                                                      int numOutputChannels,
                                                      int numSamples,
                                                      const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (auto* out = outputChannelData[ch]) juce::FloatVectorOperations::clear (out, numSamples);
}

void MainComponent::refreshLoopbackList()
{
	loopbackBox.clear();
   #if JUCE_WINDOWS
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


