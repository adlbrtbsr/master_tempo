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
            periodSec = newPeriod;
        else
            periodSec = juce::jlimit(periodSec - 0.05, periodSec + 0.05, newPeriod);
    }

    void onOnsets(const std::vector<double>& onsetTimesSec)
    {
        // Adjust phase to nearest onset
        if (periodSec <= 0.0 || onsetTimesSec.empty()) return;
        const double now = onsetTimesSec.back();
        if (!hasPhase)
        {
            phaseOriginSec = now;
            hasPhase = true;
            return;
        }
        const double phase = std::fmod(now - phaseOriginSec, periodSec);
        const double error = phase > periodSec * 0.5 ? phase - periodSec : phase; // wrap to [-T/2, T/2]
        phaseOriginSec += error * 0.5; // simple PLL correction
    }

    double getNextBeatTimeSec(double currentTimeSec) const
    {
        if (!hasPhase || periodSec <= 0.0) return -1.0;
        const double n = std::ceil((currentTimeSec - phaseOriginSec) / periodSec);
        return phaseOriginSec + n * periodSec;
    }

private:
    double sampleRate { 48000.0 };
    double periodSec { -1.0 };
    double phaseOriginSec { 0.0 };
    bool hasPhase { false };
};



