#pragma once

#include <JuceHeader.h>
#include <limits>
#include <cmath>

class OnsetDetector {
public:
    OnsetDetector(int sampleRate, int fftSize, int hopSize)
        : OnsetDetector(sampleRate, fftSize, hopSize, 0.0f, std::numeric_limits<float>::infinity()) {}

    OnsetDetector(int sampleRate, int fftSize, int hopSize, float bandLowHz, float bandHighHz)
        : sampleRate(sampleRate), fftOrder(juce::roundToInt(std::log2(fftSize))), fft(fftOrder),
          window(fftSize), prevMag(fftSize / 2 + 1, 0.0f), hopSize(hopSize),
          bandLowHz(bandLowHz), bandHighHz(bandHighHz)
    {
        jassert((1 << fftOrder) == fftSize);
        for (int i = 0; i < fftSize; ++i)
            window[i] = 0.5f * (1.0f - std::cos(2.0f * juce::MathConstants<float>::pi * (float) i / (float) (fftSize - 1)));

        stftInput.setSize(1, fftSize);
        stftInput.clear();
        fifoBuffer.resize(fftSize * 2);
    }

    void pushAudio(const float* mono, int numSamples)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            fifoBuffer[fifoWrite] = mono[i];
            fifoWrite = (fifoWrite + 1) % fifoBuffer.size();
            samplesSinceHop++;
            if (samplesSinceHop >= hopSize)
            {
                samplesSinceHop = 0;
                computeFrame();
            }
        }
    }

    void fetchNewFlux(std::vector<float>& out)
    {
        out.insert(out.end(), newFluxFrames.begin(), newFluxFrames.end());
        newFluxFrames.clear();
    }

    void fetchOnsets(std::vector<double>& out)
    {
        out.insert(out.end(), onsetTimesSec.begin(), onsetTimesSec.end());
        onsetTimesSec.clear();
    }

private:
    void computeFrame()
    {
        const int fftSize = 1 << fftOrder;
        auto* buf = stftInput.getWritePointer(0);
        for (int i = 0; i < fftSize; ++i)
        {
            int idx = (fifoWrite + fifoBuffer.size() - fftSize + i) % fifoBuffer.size();
            buf[i] = fifoBuffer[idx] * window[i];
        }

        tempFFT.resize((size_t) fftSize * 2);
        std::fill(tempFFT.begin(), tempFFT.end(), 0.0f);
        memcpy(tempFFT.data(), buf, sizeof(float) * (size_t) fftSize);
        fft.performRealOnlyForwardTransform(tempFFT.data());

        const int bins = fftSize / 2 + 1;
        // Determine bin range for band-limited flux
        int startBin = 0;
        int endBin = bins - 1;
        if (bandLowHz > 0.0f || std::isfinite(bandHighHz))
        {
            const float hzPerBin = (float) sampleRate / (float) fftSize;
            if (bandLowHz > 0.0f)
                startBin = juce::jlimit(0, bins - 1, (int) std::ceil(bandLowHz / hzPerBin));
            if (std::isfinite(bandHighHz))
                endBin = juce::jlimit(0, bins - 1, (int) std::floor(bandHighHz / hzPerBin));
        }

        float flux = 0.0f;
        for (int k = startBin; k <= endBin; ++k)
        {
            const float re = tempFFT[(size_t) k * 2];
            const float im = tempFFT[(size_t) k * 2 + 1];
            const float mag = std::sqrt(re * re + im * im);
            const float diff = juce::jmax(0.0f, mag - prevMag[(size_t) k]);
            flux += diff;
            prevMag[(size_t) k] = mag;
        }

        // Smoothing and adaptive thresholding
        const float alpha = 0.2f;
        const float smoothed = (hasLastSmoothed ? alpha * flux + (1.0f - alpha) * lastSmoothed : flux);
        lastSmoothed = smoothed;
        hasLastSmoothed = true;

        // EMA baseline
        const float beta = 0.05f;
        emaBaseline = (hasEma ? beta * smoothed + (1.0f - beta) * emaBaseline : smoothed);
        hasEma = true;
        const float threshold = emaBaseline + 0.02f * emaBaseline + 0.001f; // small offset

        // Local peak detect on smoothed
        prev2 = prev1;
        prev1 = curr;
        curr = smoothed;

        if (framesProcessed >= 2)
        {
            const bool isPeak = (prev1 > prev2) && (prev1 > curr) && (prev1 > threshold);
            if (isPeak)
            {
                const double timeSec = ((double) (framesProcessed - 1) * (double) hopSize) / (double) sampleRate;
                onsetTimesSec.push_back(timeSec);
            }
        }

        newFluxFrames.push_back(smoothed);
        ++framesProcessed;
    }

    int sampleRate;
    int fftOrder;
    juce::dsp::FFT fft;
    juce::AudioBuffer<float> stftInput;
    std::vector<float> tempFFT;
    std::vector<float> window;
    std::vector<float> prevMag;
    std::vector<float> fifoBuffer;
    size_t fifoWrite { 0 };
    int hopSize { 256 };
    int samplesSinceHop { 0 };
    // Smoothed flux stream
    bool hasLastSmoothed { false };
    float lastSmoothed { 0.0f };
    bool hasEma { false };
    float emaBaseline { 0.0f };
    float prev2 { 0.0f }, prev1 { 0.0f }, curr { 0.0f };
    uint64_t framesProcessed { 0 };
    std::vector<float> newFluxFrames;
    std::vector<double> onsetTimesSec;
    // Band-limiting
    float bandLowHz { 0.0f };
    float bandHighHz { std::numeric_limits<float>::infinity() };
};


