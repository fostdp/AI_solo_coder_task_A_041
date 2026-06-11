#pragma once

#include <vector>
#include <string>
#include <memory>
#include <random>
#include <mutex>
#include <deque>
#include <unordered_map>
#include "data_structures.h"

namespace turbine_monitor {

struct OperatingCondition {
    float head;
    float load_percent;
    float speed_rpm;
    float guide_vane_opening;
    float flow_rate;

    OperatingCondition()
        : head(0), load_percent(0), speed_rpm(0),
          guide_vane_opening(0), flow_rate(0) {}

    uint32_t bucketKey() const {
        uint32_t hBucket = static_cast<uint32_t>(head / 10.0f);
        uint32_t lBucket = static_cast<uint32_t>(load_percent / 20.0f);
        return hBucket * 100 + lBucket;
    }
};

class OperatingConditionNormalizer {
public:
    struct FeatureStats {
        float mean = 0.0f;
        float stddev = 1.0f;
        uint32_t count = 0;
    };

    void update(const std::vector<float>& features, const OperatingCondition& condition);
    std::vector<float> normalize(const std::vector<float>& features,
                                  const OperatingCondition& condition) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, std::vector<FeatureStats>> bucketStats_;
    static constexpr float MIN_STDDEV = 1e-6f;
    static constexpr uint32_t MIN_SAMPLES_FOR_STATS = 10;

    void updateStats(std::vector<FeatureStats>& stats, const std::vector<float>& features);
    std::vector<float> applyZScore(const std::vector<float>& features,
                                    const std::vector<FeatureStats>& stats) const;
};

class AdaptiveThreshold {
public:
    struct ThresholdState {
        float incipient;
        float critical;
        float developed;
        float ewmaBaseline;
        float ewmaStd;
        std::deque<float> recentScores;
        float percentile95;
        float percentile99;
        uint32_t updateCount;
    };

    AdaptiveThreshold(float baseIncipient = 0.3f,
                      float baseCritical = 0.6f,
                      float baseDeveloped = 0.8f,
                      float ewmaAlpha = 0.05f,
                      size_t windowSize = 1000);

    void update(float score, uint8_t turbineId, const OperatingCondition& condition);
    ThresholdState getThresholds(uint8_t turbineId, const OperatingCondition& condition) const;
    CavitationStage classify(float score, uint8_t turbineId,
                             const OperatingCondition& condition) const;

private:
    float baseIncipient_;
    float baseCritical_;
    float baseDeveloped_;
    float ewmaAlpha_;
    size_t windowSize_;

    mutable std::mutex mutex_;
    std::unordered_map<uint32_t, ThresholdState> stateMap_;

    uint32_t stateKey(uint8_t turbineId, const OperatingCondition& condition) const;
    float computePercentile(const std::deque<float>& data, float percentile) const;
};

class IsolationForest {
public:
    struct Node {
        bool isLeaf;
        int featureIndex;
        double threshold;
        int leftChild;
        int rightChild;
        double depth;
    };

    struct Tree {
        std::vector<Node> nodes;
        int root;
    };

    IsolationForest(int numTrees = 100, int subSamplingSize = 256);
    ~IsolationForest() = default;

    void fit(const std::vector<std::vector<float>>& data);
    double anomalyScore(const std::vector<float>& sample) const;
    bool loadModel(const std::string& path);
    bool saveModel(const std::string& path) const;

private:
    int numTrees_;
    int subSamplingSize_;
    std::vector<Tree> trees_;
    mutable std::mt19937 rng_;
    mutable std::mutex mutex_;

    int buildTree(const std::vector<std::vector<float>>& data,
                  const std::vector<int>& indices,
                  int depth,
                  Tree& tree);

    double pathLength(const std::vector<float>& sample,
                      const Tree& tree,
                      int nodeIndex) const;

    static double harmonicNumber(int n);
    static double cFactor(int n);
};

class AutoEncoder {
public:
    struct Layer {
        std::vector<std::vector<float>> weights;
        std::vector<float> biases;
        std::string activation;
    };

    AutoEncoder();
    ~AutoEncoder() = default;

    bool loadModel(const std::string& path);
    std::vector<float> forward(const std::vector<float>& input) const;
    double reconstructionError(const std::vector<float>& input) const;

private:
    std::vector<Layer> encoder_;
    std::vector<Layer> decoder_;
    int inputSize_;
    int latentSize_;
    mutable std::mutex mutex_;

    static std::vector<float> activate(const std::vector<float>& x, const std::string& activation);
    static std::vector<float> fullyConnected(const std::vector<float>& input,
                                             const Layer& layer);
};

class CavitationDetector {
public:
    CavitationDetector(bool enableAutoEncoder = true,
                       bool enableIsolationForest = true,
                       const std::string& autoencoderPath = "",
                       const std::string& isolationForestPath = "");
    ~CavitationDetector() = default;

    CavitationState detect(
        const SpectrumFeatures& spectrum,
        const WaveletFeatures& wavelet,
        uint64_t timestamp,
        uint8_t turbineId,
        uint8_t bladeId,
        const OperatingCondition& condition = OperatingCondition());

    bool loadModels();
    void setThresholds(float incipientThreshold, float criticalThreshold, float developedThreshold);

private:
    std::unique_ptr<AutoEncoder> autoEncoder_;
    std::unique_ptr<IsolationForest> isolationForest_;
    std::unique_ptr<OperatingConditionNormalizer> normalizer_;
    std::unique_ptr<AdaptiveThreshold> adaptiveThreshold_;

    bool enableAutoEncoder_;
    bool enableIsolationForest_;
    std::string autoencoderPath_;
    std::string isolationForestPath_;

    float incipientThreshold_;
    float criticalThreshold_;
    float developedThreshold_;

    std::vector<float> buildFeatureVector(
        const SpectrumFeatures& spectrum,
        const WaveletFeatures& wavelet);

    float computeCavitationIntensity(float combinedScore);
    float computeConfidence(float combinedScore, ModelType modelType);
};

}
