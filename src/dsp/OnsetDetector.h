#pragma once

#include <JuceHeader.h>
#include <limits>
#include <cmath>
#include <mutex>
#include <deque>
#include <algorithm>

class OnsetDetector {
public:
    OnsetDetector(int sampleRate, int fftSize, int hopSize)
        : OnsetDetector(sampleRate, fftSize, hopSize, 0.0f, std::numeric_limits<float>::infinity()) {}

    OnsetDetector(int sampleRate, int fftSize, int hopSize, float bandLowHz, float bandHighHz)
        : sampleRate(sampleRate), fftOrder(juce::roundToInt(std::log2(fftSize))), fft(fftOrder),
          window(fftSize), prevMag(fftSize / 2 + 1, 0.0f), prevRe(fftSize / 2 + 1, 0.0f), prevIm(fftSize / 2 + 1, 0.0f), hopSize(hopSize),
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
        std::lock_guard<std::mutex> lock(queueMutex);
        if (!newFluxFrames.empty())
        {
            out.insert(out.end(), newFluxFrames.begin(), newFluxFrames.end());
            newFluxFrames.clear();
        }
    }

    void fetchOnsets(std::vector<double>& out)
    {
        std::lock_guard<std::mutex> lock(queueMutex);
        if (!onsetTimesSec.empty())
        {
            out.insert(out.end(), onsetTimesSec.begin(), onsetTimesSec.end());
            onsetTimesSec.clear();
        }
    }

    // Update refractory window (in seconds). Caller can adapt this using current tempo.
    void setRefractorySeconds(double seconds)
    {
        refractorySec = seconds;
    }

    // Set the thresholding window length in seconds; converts to frames using hop size
    void setThresholdWindowSeconds(double seconds)
    {
        const double frames = seconds * (double) sampleRate / (double) juce::jmax(1, hopSize);
        thrWindow = juce::jlimit(16, 1024, (int) std::round(frames));
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
            // Complex-domain flux: positive increase relative to previous vector orientation
            const float prevMagK = prevMag[(size_t) k];
            const float denom = juce::jmax(1.0e-12f, prevMagK);
            const float dot = re * prevRe[(size_t) k] + im * prevIm[(size_t) k];
            const float cosDelta = (prevMagK > 1.0e-12f && mag > 1.0e-12f) ? (dot / (mag * prevMagK)) : 1.0f;
            const float term = mag - prevMagK * cosDelta;
            const float diff = juce::jmax(0.0f, term);
            flux += diff;
            prevMag[(size_t) k] = mag;
            prevRe[(size_t) k] = re;
            prevIm[(size_t) k] = im;
        }

        // Smoothing
        const float alpha = 0.2f;
        const float smoothed = (hasLastSmoothed ? alpha * flux + (1.0f - alpha) * lastSmoothed : flux);
        lastSmoothed = smoothed;
        hasLastSmoothed = true;

        // EWMA mean/std for z-normalization (more stable scaling)
        const float gamma = 0.05f; // EWMA factor
        if (!hasEwma)
        {
            ewmaMean = smoothed;
            ewmaVar = 0.0f;
            hasEwma = true;
        }
        else
        {
            const float diffm = smoothed - ewmaMean;
            ewmaMean += gamma * diffm;
            ewmaVar = (1.0f - gamma) * (ewmaVar + gamma * diffm * diffm);
        }
        const float ewmaStd = std::sqrt(juce::jmax(ewmaVar, 1.0e-12f));
        const float z = (smoothed - ewmaMean) / ewmaStd;

        // Rolling median + MAD threshold on z-scores
        recentZ.push_back(z);
        if (recentZ.size() > (size_t) thrWindow)
            recentZ.pop_front();

        float threshold = 2.5f; // default if not enough history
        if (recentZ.size() >= 9)
        {
            std::vector<float> tmp(recentZ.begin(), recentZ.end());
            const size_t mid = tmp.size() / 2;
            std::nth_element(tmp.begin(), tmp.begin() + (long) mid, tmp.end());
            const float med = tmp[mid];
            for (auto& v : tmp) v = std::abs(v - med);
            std::nth_element(tmp.begin(), tmp.begin() + (long) mid, tmp.end());
            const float mad = tmp[mid] + 1.0e-6f;
            threshold = med + thrK * 1.4826f * mad; // 1.4826 ~ Gaussian MAD->sigma
        }

        // Local peak detect on smoothed
        prev2 = prev1;
        prev1 = curr;
        curr = z;

        if (framesProcessed >= 2)
        {
            const bool isPeak = (prev1 > prev2) && (prev1 > curr) && (prev1 > threshold);
            if (isPeak)
            {
                // Quadratic (parabolic) interpolation around the peak using prev2, prev1, curr
                // Vertex offset in samples relative to the central point (prev1)
                const float denom = (prev2 - 2.0f * prev1 + curr);
                float delta = 0.0f;
                if (std::abs(denom) > 1.0e-12f)
                    delta = 0.5f * (prev2 - curr) / denom;
                delta = juce::jlimit(-1.0f, 1.0f, delta);

                const double frameIndex = (double) (framesProcessed - 1) + (double) delta;
                // Center-of-window correction: reference peaks to the middle of the analysis window
                const double centerCorrection = 0.5 * (double) fftSize;
                const double timeSec = ((frameIndex * (double) hopSize) + centerCorrection) / (double) sampleRate;
                // Refractory: ignore onsets within a short window
                // Tempo-adaptive refractory: if we have an estimate, expand refractory up to 20% of period
                const double adaptiveRef = juce::jlimit(0.05, 0.15, refractorySec);
                const double minGapSec = adaptiveRef;
                const bool allow = (!hasLastOnsetSec || (timeSec - lastOnsetSec) >= minGapSec);
                if (allow)
                {
                    {
                        std::lock_guard<std::mutex> lock(queueMutex);
                        onsetTimesSec.push_back(timeSec);
                    }
                    lastOnsetSec = timeSec;
                    hasLastOnsetSec = true;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(queueMutex);
            newFluxFrames.push_back(z);
        }
        ++framesProcessed;
    }

    int sampleRate;
    int fftOrder;
    juce::dsp::FFT fft;
    juce::AudioBuffer<float> stftInput;
    std::vector<float> tempFFT;
    std::vector<float> window;
    std::vector<float> prevMag;
    std::vector<float> prevRe;
    std::vector<float> prevIm;
    std::vector<float> fifoBuffer;
    size_t fifoWrite { 0 };
    int hopSize { 256 };
    int samplesSinceHop { 0 };
    // Smoothed flux stream
    bool hasLastSmoothed { false };
    float lastSmoothed { 0.0f };
    // EWMA for z-normalization
    bool hasEwma { false };
    float ewmaMean { 0.0f };
    float ewmaVar { 0.0f };
    float prev2 { 0.0f }, prev1 { 0.0f }, curr { 0.0f };
    uint64_t framesProcessed { 0 };
    std::vector<float> newFluxFrames;
    std::vector<double> onsetTimesSec;
    std::mutex queueMutex;
    // Band-limiting
    float bandLowHz { 0.0f };
    float bandHighHz { std::numeric_limits<float>::infinity() };
    // Thresholding state
    std::deque<float> recentZ;
    int thrWindow { 64 };
    float thrK { 3.0f };
    // Refractory
    double refractorySec { 0.06 };
    double lastOnsetSec { 0.0 };
    bool hasLastOnsetSec { false };
};


