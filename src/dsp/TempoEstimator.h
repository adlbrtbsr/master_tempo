#pragma once

#include <JuceHeader.h>
#include <deque>
#include <numeric>
#include <complex>

class TempoEstimator {
public:
    explicit TempoEstimator(double sampleRate, int hopSize)
        : sampleRate(sampleRate), hopSize(hopSize) {}

    // Append new flux frames; compute BPM using autocorrelation over a window
    void appendFlux(const std::vector<float>& newFlux)
    {
        flux.insert(flux.end(), newFlux.begin(), newFlux.end());
        // Adapt memory to current tempo if available: cover ~8–12 beats
        size_t maxFrames = memoryFrames; // base memory
        if (bpm > 0.0)
        {
            const double framesPerSecond = sampleRate / (double) hopSize;
            const double periodSec = 60.0 / bpm;
            const double targetSec = juce::jlimit(4.0, 20.0, 10.0 * periodSec); // aim ~10 beats
            maxFrames = (size_t) juce::jlimit<double>(512.0, 8192.0, std::round(targetSec * framesPerSecond));
        }
        if (flux.size() > maxFrames)
            flux.erase(flux.begin(), flux.begin() + (flux.size() - maxFrames));
        estimate();
    }

    // Ingest newly detected onsets (absolute times in seconds)
    void ingestOnsets(const std::vector<double>& onsetTimesSec)
    {
        for (double t : onsetTimesSec)
        {
            recentOnsets.push_back(t);
            if (recentOnsets.size() > maxRecentOnsets)
                recentOnsets.pop_front();
        }
    }

    double getBpm() const { return bpm; }
    double getConfidence() const { return confidence; }
    const std::vector<std::pair<double, double>>& getLastCandidates() const { return lastCandidates; } // (bpm, score)
    // Runtime tuning
    void setTopKCandidates(int k)               { topKCandidates = juce::jlimit(1, 10, k); }
    void setIoiWeight(double w)                 { ioiWeight = juce::jlimit(0.0, 4.0, w); }
    void setMaxRecentOnsets(size_t n)           { maxRecentOnsets = juce::jlimit<size_t>(8, 256, n); }
    void setMemoryFrames(size_t frames)         { memoryFrames = juce::jlimit<size_t>(512, 8192, frames); }
    void setSlewPercent(double pct)             { slewPercent = juce::jlimit(0.01, 0.20, pct); }

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

        float energy0 = std::inner_product(x.begin(), x.end(), x.begin(), 0.0f);
        if (energy0 <= 1e-9f) return;

        // FFT-based autocorrelation via convolution theorem using JUCE FFT (preallocated)
        const size_t n = x.size();
        size_t needed = 1; while (needed < 2 * n) needed <<= 1;
        ensureFftSize((int) needed);
        // zero pad and copy to real buffer (JUCE real-only FFT uses a 2 * fftSize buffer)
        std::fill(fftBuffer.begin(), fftBuffer.end(), 0.0f);
        std::copy(x.begin(), x.end(), fftBuffer.begin());
        // forward real FFT (interleaved re,im pairs in fftBuffer)
        fft->performRealOnlyForwardTransform(fftBuffer.data());
        // compute power spectrum in-place
        const int bins = (int) (fftSize / 2);
        for (int k = 0; k <= bins; ++k)
        {
            const float re = fftBuffer[(size_t) k * 2];
            const float im = fftBuffer[(size_t) k * 2 + 1];
            const float p = re * re + im * im;
            fftBuffer[(size_t) k * 2] = p;
            fftBuffer[(size_t) k * 2 + 1] = 0.0f;
        }
        // inverse
        fft->performRealOnlyInverseTransform(fftBuffer.data());
        std::vector<float> acf(n, 0.0f);
        for (size_t i = 0; i < n; ++i) acf[i] = fftBuffer[i];
        // Bias-corrected autocorrelation to reduce short-lag bias: divide by (n - lag)
        for (size_t lag = 1; lag < n; ++lag)
        {
            const float denom = (float) juce::jmax<size_t>(1, n - lag);
            acf[lag] /= denom;
        }

        // Energy-normalized ACF (optional) and positive lags only
        // Collect local maxima across lag range and evaluate top-K with IOI support
        struct Peak { float score; int lag; double bpm; };
        std::vector<Peak> peaks;
        peaks.reserve(64);
        auto scoreAtLag = [&acf](int lag)->float { return (lag >= 0 && (size_t)lag < acf.size()) ? acf[(size_t) lag] : 0.0f; };
        float prev = 0.0f, curr = 0.0f;
        curr = scoreAtLag(minLag);
        float next = scoreAtLag(minLag + 1);
        for (int lag = minLag + 1; lag < maxLag; ++lag)
        {
            prev = curr;
            curr = next;
            next = scoreAtLag(lag + 1);
            const bool isLocalMax = curr > prev && curr >= next;
            if (!isLocalMax) continue;
            const double bpmCand = lagToBpm(lag, framesPerSecond);
            const double w = metricalWeight(bpmCand);
            const float weighted = (float) (curr * w);
            if (weighted > 0.0f)
                peaks.push_back(Peak{ weighted, lag, bpmCand });
        }

        if (peaks.empty()) return;
        // Keep top-K by weighted score, then perform harmonic merging of related tempi
        const size_t K = juce::jmin((size_t) topKCandidates, peaks.size());
        std::partial_sort(peaks.begin(), peaks.begin() + (long) K, peaks.end(), [](const Peak& a, const Peak& b){ return a.score > b.score; });

        // Evaluate candidates with IOI support
        lastCandidates.clear();
        double bestTotal = -1.0;
        int bestLag = -1;
        float bestScore = 0.0f;
        double bestBpm = -1.0;
        // Merge harmonically related candidates (0.5x, 1x, 2x, 3x) and pick the best group
        struct Group { double reprBpm; double totalScore; int reprLag; float reprScore; };
        std::vector<Group> groups;
        auto isHarmonic = [](double a, double b){
            // Compare on log-frequency axis; accept ratios near {1/2, 2/3, 3/4, 1, 4/3, 3/2, 2, 3}
            const double hi = juce::jmax(a, b);
            const double lo = juce::jmin(a, b);
            if (lo <= 0.0) return false;
            const double r = hi / lo;
            const double targets[] = { 0.5, 2.0/3.0, 3.0/4.0, 1.0, 4.0/3.0, 3.0/2.0, 2.0, 3.0 };
            for (double t : targets)
            {
                const double diff = std::abs(r - t);
                if (diff < 0.06) return true; // within 6%
            }
            return false;
        };

        // Pre-score each peak
        std::vector<double> peakTotals(K, 0.0);
        for (size_t i = 0; i < K; ++i)
        {
            const auto& pk = peaks[i];
            const double support = ioiSupportForBpm(pk.bpm);
            double continuity = 1.0;
            if (bpm > 0.0)
            {
                const double rel = std::abs(pk.bpm - bpm) / juce::jmax(1.0, bpm);
                continuity = std::exp(-4.0 * rel);
            }
            peakTotals[i] = (double) pk.score * (1.0 + ioiWeight * support) * continuity;
        }
        std::vector<bool> used(K, false);
        for (size_t i = 0; i < K; ++i)
        {
            if (used[i]) continue;
            Group g { peaks[i].bpm, peakTotals[i], peaks[i].lag, peaks[i].score };
            used[i] = true;
            for (size_t j = i + 1; j < K; ++j)
            {
                if (used[j]) continue;
                if (isHarmonic(peaks[i].bpm, peaks[j].bpm))
                {
                    g.totalScore += 0.75 * peakTotals[j]; // discounted add for harmonics
                    used[j] = true;
                }
            }
            groups.push_back(g);
        }

        lastCandidates.clear();
        bestTotal = -1.0; bestLag = -1; bestScore = 0.0f; bestBpm = -1.0;
        for (const auto& g : groups)
        {
            lastCandidates.emplace_back(g.reprBpm, g.totalScore);
            if (g.totalScore > bestTotal)
            {
                bestTotal = g.totalScore;
                bestLag = g.reprLag;
                bestScore = g.reprScore;
                bestBpm = g.reprBpm;
            }
        }

        if (bestLag > 0 && bestBpm > 0.0)
        {
            const double newBpm = bestBpm;
            // Proportional slew limit per update
            if (bpm <= 0.0)
            {
                bpm = newBpm;
            }
            else
            {
                const double step = slewPercent * juce::jmax(1.0, bpm);
                bpm = juce::jlimit(bpm - step, bpm + step, newBpm);
            }

            const double confAcf = juce::jlimit(0.0, 1.0, (double) bestScore / (double) energy0);
            const double confIoi = ioiSupportForBpm(bpm);
            confidence = juce::jlimit(0.0, 1.0, 0.5 * confAcf + 0.5 * confIoi);
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
        const double spread = 80.0; // flatter prior to reduce bias
        const double w = std::exp(-std::pow((bpm - center) / spread, 2.0));
        return 0.7 + 0.3 * w; // reduce influence of the prior
    }

    // Compute support of a BPM by checking inter-onset intervals near multiples of the beat period
    double ioiSupportForBpm(double bpmCand) const
    {
        if (recentOnsets.size() < 3 || bpmCand <= 0.0) return 0.0;
        const double period = 60.0 / bpmCand;
        const double tol = juce::jlimit(0.012, 0.080, 0.12 * period); // relaxed tolerance: up to 12% of period

        // Build IOIs using all pairwise differences within a recent window
        std::vector<double> iois;
        const size_t N = recentOnsets.size();
        for (size_t i = 0; i < N; ++i)
        {
            for (size_t j = i + 1; j < N; ++j)
            {
                const double d = recentOnsets[j] - recentOnsets[i];
                if (d > 0.02 && d < 3.0) // ignore implausible gaps
                    iois.push_back(d);
            }
        }
        if (iois.empty()) return 0.0;
        // Trim IOI outliers using IQR fence
        std::sort(iois.begin(), iois.end());
        const auto qIndex = [&](double q){ return (size_t) juce::jlimit<double>(0.0, (double) (iois.size()-1), q * (iois.size()-1)); };
        const double q1 = iois[qIndex(0.25)];
        const double q3 = iois[qIndex(0.75)];
        const double iqr = juce::jmax(1.0e-6, q3 - q1);
        const double lo = q1 - 1.5 * iqr;
        const double hi = q3 + 1.5 * iqr;
        std::vector<double> trimmed; trimmed.reserve(iois.size());
        for (double d : iois) if (d >= lo && d <= hi) trimmed.push_back(d);
        if (trimmed.size() >= 3) iois.swap(trimmed);

        int hits = 0;
        for (double d : iois)
        {
            const int k = juce::jlimit(1, 6, (int) std::round(d / period));
            const double target = (double) k * period;
            if (std::abs(d - target) <= tol)
                ++hits;
        }
        return (double) hits / (double) iois.size();
    }

    double sampleRate { 48000.0 };
    int hopSize { 256 };
    std::vector<float> flux;
    double bpm { -1.0 };
    double confidence { 0.0 };
    std::deque<double> recentOnsets;
    size_t maxRecentOnsets { 64 };
    int topKCandidates { 5 };
    double ioiWeight { 1.0 };
    std::vector<std::pair<double, double>> lastCandidates;
    size_t memoryFrames { 2048 };
    double slewPercent { 0.03 }; // 3% per update
    // Hysteresis
    int stableCandCount { 0 };

    // JUCE FFT buffers (preallocated)
    std::unique_ptr<juce::dsp::FFT> fft;
    int fftOrder { 0 };
    int fftSize { 0 };
    std::vector<float> fftBuffer; // size = 2 * fftSize for JUCE real-only FFT

    void ensureFftSize(int minSize)
    {
        int order = 0; int size = 1;
        while (size < minSize) { size <<= 1; ++order; }
        if (size != fftSize)
        {
            fftSize = size;
            fftOrder = order;
            fft = std::make_unique<juce::dsp::FFT>(fftOrder);
            fftBuffer.resize((size_t) (2 * fftSize));
        }
    }
};



