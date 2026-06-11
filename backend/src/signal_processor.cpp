#include "signal_processor.h"
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstring>

namespace turbine_monitor {

SignalProcessor::SignalProcessor()
    : fftPlan_(nullptr), fftSize_(0) {
}

SignalProcessor::~SignalProcessor() {
    if (fftPlan_) {
        fftw_destroy_plan(fftPlan_);
    }
    fftw_cleanup();
}

std::vector<std::complex<double>> SignalProcessor::fft(const std::vector<float>& signal) {
    int n = signal.size();
    std::vector<double> in(n);
    for (int i = 0; i < n; ++i) {
        in[i] = signal[i];
    }

    std::vector<fftw_complex> out((n / 2) + 1);
    fftw_plan plan = fftw_plan_dft_r2c_1d(n, in.data(), out.data(), FFTW_ESTIMATE);
    fftw_execute(plan);
    fftw_destroy_plan(plan);

    std::vector<std::complex<double>> result(n);
    for (int i = 0; i <= n / 2; ++i) {
        result[i] = std::complex<double>(out[i][0], out[i][1]);
    }
    for (int i = 1; i < n / 2; ++i) {
        result[n - i] = std::conj(result[i]);
    }

    return result;
}

std::vector<float> SignalProcessor::computePowerSpectrum(const std::vector<std::complex<double>>& fftResult) {
    int n = fftResult.size();
    std::vector<float> powerSpectrum(n / 2 + 1);
    for (int i = 0; i <= n / 2; ++i) {
        double mag = std::abs(fftResult[i]);
        powerSpectrum[i] = static_cast<float>((mag * mag) / n);
    }
    return powerSpectrum;
}

float SignalProcessor::findPeakFrequency(const std::vector<float>& powerSpectrum, uint32_t sampleRate) {
    if (powerSpectrum.empty()) return 0.0f;

    size_t maxIdx = std::distance(powerSpectrum.begin(),
        std::max_element(powerSpectrum.begin() + 1, powerSpectrum.end()));

    float freqResolution = static_cast<float>(sampleRate) / (2.0f * (powerSpectrum.size() - 1));
    return static_cast<float>(maxIdx) * freqResolution;
}

float SignalProcessor::computeRMS(const std::vector<float>& signal) {
    if (signal.empty()) return 0.0f;

    double sumSq = 0.0;
    for (float v : signal) {
        sumSq += static_cast<double>(v) * v;
    }
    return static_cast<float>(std::sqrt(sumSq / signal.size()));
}

float SignalProcessor::computeCrestFactor(const std::vector<float>& signal, float rms) {
    if (signal.empty() || rms == 0.0f) return 0.0f;

    float maxAbs = 0.0f;
    for (float v : signal) {
        maxAbs = std::max(maxAbs, std::abs(v));
    }
    return maxAbs / rms;
}

float SignalProcessor::computeKurtosis(const std::vector<float>& signal) {
    if (signal.empty()) return 0.0f;

    double mean = std::accumulate(signal.begin(), signal.end(), 0.0) / signal.size();

    double m2 = 0.0, m4 = 0.0;
    for (float v : signal) {
        double diff = v - mean;
        m2 += diff * diff;
        m4 += std::pow(diff, 4);
    }
    m2 /= signal.size();
    m4 /= signal.size();

    if (m2 == 0.0) return 0.0f;
    return static_cast<float>(m4 / (m2 * m2) - 3.0);
}

float SignalProcessor::computeSkewness(const std::vector<float>& signal) {
    if (signal.empty()) return 0.0f;

    double mean = std::accumulate(signal.begin(), signal.end(), 0.0) / signal.size();

    double m2 = 0.0, m3 = 0.0;
    for (float v : signal) {
        double diff = v - mean;
        m2 += diff * diff;
        m3 += diff * diff * diff;
    }
    m2 /= signal.size();
    m3 /= signal.size();

    if (m2 == 0.0) return 0.0f;
    return static_cast<float>(m3 / std::pow(m2, 1.5));
}

float SignalProcessor::computeSpectralCentroid(const std::vector<float>& powerSpectrum, uint32_t sampleRate) {
    if (powerSpectrum.empty()) return 0.0f;

    float freqResolution = static_cast<float>(sampleRate) / (2.0f * (powerSpectrum.size() - 1));

    double weightedSum = 0.0;
    double totalPower = 0.0;
    for (size_t i = 0; i < powerSpectrum.size(); ++i) {
        weightedSum += i * freqResolution * powerSpectrum[i];
        totalPower += powerSpectrum[i];
    }

    return totalPower > 0 ? static_cast<float>(weightedSum / totalPower) : 0.0f;
}

float SignalProcessor::computeSpectralBandwidth(const std::vector<float>& powerSpectrum, uint32_t sampleRate) {
    if (powerSpectrum.empty()) return 0.0f;

    float centroid = computeSpectralCentroid(powerSpectrum, sampleRate);
    float freqResolution = static_cast<float>(sampleRate) / (2.0f * (powerSpectrum.size() - 1));

    double sum = 0.0;
    double totalPower = 0.0;
    for (size_t i = 0; i < powerSpectrum.size(); ++i) {
        float freq = i * freqResolution;
        sum += (freq - centroid) * (freq - centroid) * powerSpectrum[i];
        totalPower += powerSpectrum[i];
    }

    return totalPower > 0 ? static_cast<float>(std::sqrt(sum / totalPower)) : 0.0f;
}

float SignalProcessor::computeBandEnergy(const std::vector<float>& powerSpectrum,
                                          uint32_t sampleRate,
                                          float freqLow,
                                          float freqHigh) {
    if (powerSpectrum.empty()) return 0.0f;

    float freqResolution = static_cast<float>(sampleRate) / (2.0f * (powerSpectrum.size() - 1));
    size_t idxLow = static_cast<size_t>(freqLow / freqResolution);
    size_t idxHigh = static_cast<size_t>(freqHigh / freqResolution);

    idxLow = std::min(idxLow, powerSpectrum.size() - 1);
    idxHigh = std::min(idxHigh, powerSpectrum.size() - 1);
    idxHigh = std::max(idxHigh, idxLow);

    double energy = 0.0;
    for (size_t i = idxLow; i <= idxHigh; ++i) {
        energy += powerSpectrum[i];
    }
    return static_cast<float>(energy);
}

SpectrumFeatures SignalProcessor::extractSpectrumFeatures(
    const std::vector<float>& signal,
    uint32_t sampleRate,
    uint64_t timestamp,
    uint8_t turbineId,
    SensorType sensorType,
    uint8_t sensorId,
    uint8_t bladeId) {

    SpectrumFeatures features{};
    features.timestamp = timestamp;
    features.turbine_id = turbineId;
    features.sensor_type = sensorType;
    features.sensor_id = sensorId;
    features.blade_id = bladeId;

    if (signal.size() < 2) {
        return features;
    }

    auto fftResult = fft(signal);
    auto powerSpectrum = computePowerSpectrum(fftResult);

    features.peak_frequency = findPeakFrequency(powerSpectrum, sampleRate);
    features.rms_value = computeRMS(signal);
    features.crest_factor = computeCrestFactor(signal, features.rms_value);
    features.kurtosis = computeKurtosis(signal);
    features.skewness = computeSkewness(signal);
    features.spectral_centroid = computeSpectralCentroid(powerSpectrum, sampleRate);
    features.spectral_bandwidth = computeSpectralBandwidth(powerSpectrum, sampleRate);

    float nyquist = sampleRate / 2.0f;
    features.band_energy_low = computeBandEnergy(powerSpectrum, sampleRate, 0.0f, nyquist * 0.25f);
    features.band_energy_mid = computeBandEnergy(powerSpectrum, sampleRate, nyquist * 0.25f, nyquist * 0.75f);
    features.band_energy_high = computeBandEnergy(powerSpectrum, sampleRate, nyquist * 0.75f, nyquist);

    float totalEnergy = features.band_energy_low + features.band_energy_mid + features.band_energy_high;
    features.harmonic_ratio = totalEnergy > 0 ? features.band_energy_mid / totalEnergy : 0.0f;

    return features;
}

SignalProcessor::WaveletFilter SignalProcessor::getWaveletFilter(const std::string& waveletBasis) {
    WaveletFilter filter;

    if (waveletBasis == "db4") {
        double sqrt3 = std::sqrt(3.0);
        double c = 1.0 / (4.0 * std::sqrt(2.0));

        filter.lowPassDecomp = {
            (1 + sqrt3) * c, (3 + sqrt3) * c, (3 - sqrt3) * c, (1 - sqrt3) * c
        };
        filter.highPassDecomp = {
            (1 - sqrt3) * c, -(3 - sqrt3) * c, (3 + sqrt3) * c, -(1 + sqrt3) * c
        };

        filter.lowPassRecon.resize(4);
        filter.highPassRecon.resize(4);
        for (int i = 0; i < 4; ++i) {
            filter.lowPassRecon[i] = filter.lowPassDecomp[3 - i];
            filter.highPassRecon[i] = filter.highPassDecomp[3 - i];
        }
    } else {
        filter.lowPassDecomp = {0.70710678118, 0.70710678118};
        filter.highPassDecomp = {-0.70710678118, 0.70710678118};
        filter.lowPassRecon = {0.70710678118, 0.70710678118};
        filter.highPassRecon = {0.70710678118, -0.70710678118};
    }

    return filter;
}

std::vector<double> SignalProcessor::convolve(const std::vector<double>& signal,
                                              const std::vector<double>& kernel,
                                              const std::string& mode) {
    int n = signal.size();
    int m = kernel.size();
    int resultSize;

    if (mode == "same") {
        resultSize = n;
    } else if (mode == "full") {
        resultSize = n + m - 1;
    } else {
        resultSize = std::max(n - m + 1, 0);
    }

    std::vector<double> result(resultSize, 0.0);

    int start = mode == "full" ? 0 : (mode == "same" ? (m - 1) / 2 : m - 1);
    int end = mode == "full" ? n + m - 2 : (mode == "same" ? start + n - 1 : n - 1);

    for (int i = start; i <= end; ++i) {
        double sum = 0.0;
        for (int j = 0; j < m; ++j) {
            int idx = i - j;
            if (idx >= 0 && idx < n) {
                sum += signal[idx] * kernel[j];
            }
        }
        result[i - start] = sum;
    }

    return result;
}

std::vector<double> SignalProcessor::downsample(const std::vector<double>& signal, int factor) {
    std::vector<double> result;
    result.reserve(signal.size() / factor + 1);
    for (size_t i = 0; i < signal.size(); i += factor) {
        result.push_back(signal[i]);
    }
    return result;
}

std::vector<std::vector<float>> SignalProcessor::waveletPacketDecomposition(
    const std::vector<float>& signal,
    int level,
    const WaveletFilter& filter) {

    std::vector<std::vector<float>> coefficients;
    std::vector<std::vector<double>> currentLevel;

    std::vector<double> sig(signal.begin(), signal.end());
    currentLevel.push_back(sig);

    for (int l = 0; l < level; ++l) {
        std::vector<std::vector<double>> nextLevel;
        for (const auto& node : currentLevel) {
            auto lowPass = convolve(node, filter.lowPassDecomp, "same");
            auto highPass = convolve(node, filter.highPassDecomp, "same");
            auto lowDown = downsample(lowPass, 2);
            auto highDown = downsample(highPass, 2);
            nextLevel.push_back(lowDown);
            nextLevel.push_back(highDown);
        }
        currentLevel = std::move(nextLevel);
    }

    for (const auto& node : currentLevel) {
        std::vector<float> coef(node.begin(), node.end());
        coefficients.push_back(std::move(coef));
    }

    return coefficients;
}

WaveletFeatures SignalProcessor::extractWaveletFeatures(
    const std::vector<float>& signal,
    uint64_t timestamp,
    uint8_t turbineId,
    uint8_t sensorId,
    uint8_t bladeId,
    const std::string& waveletBasis,
    int decompositionLevel) {

    WaveletFeatures features{};
    features.timestamp = timestamp;
    features.turbine_id = turbineId;
    features.sensor_id = sensorId;
    features.blade_id = bladeId;
    features.wavelet_basis = waveletBasis;
    features.decomposition_level = static_cast<uint8_t>(decompositionLevel);

    if (signal.size() < (1 << decompositionLevel)) {
        return features;
    }

    WaveletFilter filter = getWaveletFilter(waveletBasis);
    auto coefficients = waveletPacketDecomposition(signal, decompositionLevel, filter);

    int numBands = coefficients.size();
    features.band_energy.resize(numBands);
    features.energy_ratio.resize(numBands);

    double totalEnergy = 0.0;
    for (int i = 0; i < numBands; ++i) {
        double energy = 0.0;
        for (float c : coefficients[i]) {
            energy += static_cast<double>(c) * c;
        }
        features.band_energy[i] = static_cast<float>(energy);
        totalEnergy += energy;
    }

    features.total_energy = static_cast<float>(totalEnergy);

    for (int i = 0; i < numBands; ++i) {
        features.energy_ratio[i] = totalEnergy > 0 ?
            features.band_energy[i] / static_cast<float>(totalEnergy) : 0.0f;
    }

    double entropy = 0.0;
    for (float ratio : features.energy_ratio) {
        if (ratio > 0) {
            entropy -= ratio * std::log2(ratio);
        }
    }
    features.energy_entropy = static_cast<float>(entropy);

    return features;
}

}
