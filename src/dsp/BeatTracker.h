#pragma once

#include <JuceHeader.h>

class BeatTracker {
public:
    explicit BeatTracker(double sampleRate)
        : sampleRate(sampleRate) {}

    void updateBpm(double newBpm)
    {
        if (newBpm <= 0.0) return;
        const double newPeriod = 60.0 / newBpm;
        if (periodSec <= 0.0)
        {
            periodSec = newPeriod;
        }
        else
        {
            const double step = juce::jmax(0.02, 0.06 * periodSec); // ~6% of current period, min 20 ms
            periodSec = juce::jlimit(periodSec - step, periodSec + step, newPeriod);
        }
    }

    void onOnsets(const std::vector<double>& onsetTimesSec)
    {
        // Adjust phase using multiple recent onsets for robustness
        if (periodSec <= 0.0 || onsetTimesSec.empty()) return;
        const size_t N = juce::jmin<size_t>(onsetTimesSec.size(), 5);
        if (!hasPhase)
        {
            phaseOriginSec = onsetTimesSec.back();
            hasPhase = true;
            return;
        }
        std::vector<double> errors;
        errors.reserve(N);
        for (size_t i = onsetTimesSec.size() - N; i < onsetTimesSec.size(); ++i)
        {
            const double t = onsetTimesSec[i];
            double phase = std::fmod(t - phaseOriginSec, periodSec);
            if (phase > periodSec * 0.5) phase -= periodSec; // wrap to [-T/2, T/2]
            errors.push_back(phase);
        }
        // Median error for robustness
        std::nth_element(errors.begin(), errors.begin() + (long) (errors.size() / 2), errors.end());
        const double medianError = errors[errors.size() / 2];
        const double k = 0.35; // proportional correction gain
        phaseOriginSec += k * medianError;
    }

    double getNextBeatTimeSec(double currentTimeSec) const
    {
        if (!hasPhase || periodSec <= 0.0) return -1.0;
        const double n = std::ceil((currentTimeSec - phaseOriginSec) / periodSec);
        return phaseOriginSec + n * periodSec;
    }

    void freezePhase() { /* placeholder for future hysteresis hooks */ }

private:
    double sampleRate { 48000.0 };
    double periodSec { -1.0 };
    double phaseOriginSec { 0.0 };
    bool hasPhase { false };
};



