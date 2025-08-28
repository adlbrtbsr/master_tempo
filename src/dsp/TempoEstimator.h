#pragma once

#include <JuceHeader.h>

class TempoEstimator {
public:
    explicit TempoEstimator(double sampleRate, int hopSize)
        : sampleRate(sampleRate), hopSize(hopSize) {}

    // Append new flux frames; compute BPM using autocorrelation over a window
    void appendFlux(const std::vector<float>& newFlux)
    {
        flux.insert(flux.end(), newFlux.begin(), newFlux.end());
        const size_t maxFrames = 4096;
        if (flux.size() > maxFrames)
            flux.erase(flux.begin(), flux.begin() + (flux.size() - maxFrames));
        estimate();
    }

    double getBpm() const { return bpm; }
    double getConfidence() const { return confidence; }

private:
    void estimate()
    {
        if (flux.size() < 256) return;

        // Normalize
        std::vector<float> x(flux.begin(), flux.end());
        const float mean = std::accumulate(x.begin(), x.end(), 0.0f) / (float) x.size();
        for (auto& v : x) v -= mean;

        const int minBpm = 40, maxBpm = 240;
        const double framesPerSecond = sampleRate / (double) hopSize;
        const int minLag = (int) std::floor(framesPerSecond * 60.0 / (double) maxBpm);
        const int maxLag = (int) std::ceil (framesPerSecond * 60.0 / (double) minBpm);
        if (maxLag >= (int) x.size()) return;

        float bestVal = -1.0f;
        int bestLag = -1;
        float energy0 = std::inner_product(x.begin(), x.end(), x.begin(), 0.0f);
        if (energy0 <= 1e-9f) return;

        for (int lag = minLag; lag <= maxLag; ++lag)
        {
            float s = 0.0f;
            for (size_t i = 0; i + (size_t) lag < x.size(); ++i)
                s += x[i] * x[i + (size_t) lag];

            // Weight to reduce 2x/0.5x ambiguity
            const double bpmCand = lagToBpm(lag, framesPerSecond);
            const double mult = metricalWeight(bpmCand);
            s = (float) (s * mult);

            if (s > bestVal)
            {
                bestVal = s;
                bestLag = lag;
            }
        }

        if (bestLag > 0)
        {
            const double newBpm = lagToBpm(bestLag, framesPerSecond);
            // Slew-limit updates
            if (bpm <= 0.0)
                bpm = newBpm;
            else
                bpm = juce::jlimit(bpm - 2.0, bpm + 2.0, newBpm);

            confidence = juce::jlimit(0.0, 1.0, (double) bestVal / (double) energy0);
        }
    }

    static double lagToBpm(int lag, double framesPerSecond)
    {
        const double periodSec = (double) lag / framesPerSecond;
        return 60.0 / periodSec;
    }

    static double metricalWeight(double bpm)
    {
        // Prefer 80–160 bpm band; downweight extremes
        if (bpm < 40.0 || bpm > 240.0) return 0.0;
        const double center = 120.0;
        const double spread = 60.0;
        const double w = std::exp(-std::pow((bpm - center) / spread, 2.0));
        return 0.5 + 0.5 * w;
    }

    double sampleRate { 48000.0 };
    int hopSize { 256 };
    std::vector<float> flux;
    double bpm { -1.0 };
    double confidence { 0.0 };
};



