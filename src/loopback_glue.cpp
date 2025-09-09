#include "MainComponent.h"

bool MainComponent::startLoopbackCaptureForEndpoint (const juce::String& outputName)
{
   #if JUCE_WINDOWS
    loopbackCapture.reset (new WASAPILoopbackCapture());
    const bool started = loopbackCapture->start (outputName, [this](const float* interleaved, int frames, int chans, double sr, double qpcSeconds)
    {
        handleLoopbackSamples (interleaved, frames, chans, sr, qpcSeconds);
    });
    usingLoopback = started;
    return started;
   #else
    juce::ignoreUnused (outputName);
    return false;
   #endif
}

void MainComponent::handleLoopbackSamples (const float* interleaved, int frames, int chans, double sr, double qpcSeconds)
{
    juce::HeapBlock<float> mono (frames);
    if (chans <= 1)
    {
        juce::FloatVectorOperations::copy (mono.get(), interleaved, frames);
    }
    else
    {
        const float invCh = 1.0f / (float) juce::jmax(1, chans);
        for (int i = 0; i < frames; ++i)
        {
            double sum = 0.0;
            for (int c = 0; c < chans; ++c)
            {
                const float s = interleaved[i * chans + c];
                sum += (double) s;
            }
            mono[i] = (float) (sum * invCh);
        }
    }

    if (currentSampleRate.load() != sr)
        prepareProcessing (sr, 512);

    int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
    fifo.prepareToWrite (frames, start1, size1, start2, size2);
    if (size1 > 0) juce::FloatVectorOperations::copy (ringBuffer.getWritePointer(0) + start1, mono.get(), size1);
    if (size2 > 0) juce::FloatVectorOperations::copy (ringBuffer.getWritePointer(0) + start2, mono.get() + size1, size2);
    fifo.finishedWrite (size1 + size2);

    capturedSamples.fetch_add((uint64_t) frames, std::memory_order_relaxed);
    lastQpcSeconds.store(qpcSeconds, std::memory_order_relaxed);
}


