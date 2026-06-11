#pragma once

#include <vector>
#include <complex>
#include <cmath>
#include <memory>
#include <fftw3.h>
#include "data_structures.h"

namespace turbine_monitor {

class SignalProcessor {
public:
    SignalProcessor();
    ~SignalProcessor();

    SpectrumFeatures extractSpectrumFeatures(
        const std::vector<float>& signal,
        uint32_t sampleRate,
        uint64_t timestamp,
        uint8_t turbineId,
        SensorType sensorType,
        uint8_t sensorId,
        uint8_t bladeId);

    WaveletFeatures extractWaveletFeatures(
        const std::vector<float>& signal,
        uint64_t timestamp,
        uint8_t turbineId,
        uint8_t sensorId,
        uint8_t bladeId,
        const std::string& waveletBasis = "db4",
        int decompositionLevel = 5);

    static std::vector<std::complex<double>> fft(const std::vector<float>& signal);
    static std::vector<float> computePowerSpectrum(const std::vector<std::complex<double>>& fftResult);
    static float findPeakFrequency(const std::vector<float>& powerSpectrum, uint32_t sampleRate);
    static float computeRMS(const std::vector<float>& signal);
    static float computeCrestFactor(const std::vector<float>& signal, float rms);
    static float computeKurtosis(const std::vector<float>& signal);
    static float computeSkewness(const std::vector<float>& signal);
    static float computeSpectralCentroid(const std::vector<float>& powerSpectrum, uint32_t sampleRate);
    static float computeSpectralBandwidth(const std::vector<float>& powerSpectrum, uint32_t sampleRate);
    static float computeBandEnergy(const std::vector<float>& powerSpectrum,
                                   uint32_t sampleRate,
                                   float freqLow,
                                   float freqHigh);

private:
    struct WaveletFilter {
        std::vector<double> lowPassDecomp;
        std::vector<double> highPassDecomp;
        std::vector<double> lowPassRecon;
        std::vector<double> highPassRecon;
    };

    static WaveletFilter getWaveletFilter(const std::string& waveletBasis);
    static std::vector<std::vector<float>> waveletPacketDecomposition(
        const std::vector<float>& signal,
        int level,
        const WaveletFilter& filter);

    static std::vector<double> convolve(const std::vector<double>& signal,
                                         const std::vector<double>& kernel,
                                         const std::string& mode);
    static std::vector<double> downsample(const std::vector<double>& signal, int factor);

    fftw_plan fftPlan_;
    std::vector<double> fftIn_;
    std::vector<fftw_complex> fftOut_;
    int fftSize_;
    mutable std::mutex fftMutex_;
};

}
