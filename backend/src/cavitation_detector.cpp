#include "cavitation_detector.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <numeric>
#include <json/json.h>

namespace turbine_monitor {

void OperatingConditionNormalizer::updateStats(
    std::vector<FeatureStats>& stats,
    const std::vector<float>& features) {

    if (stats.size() != features.size()) {
        stats.resize(features.size());
    }

    for (size_t i = 0; i < features.size(); ++i) {
        auto& s = stats[i];
        float delta = features[i] - s.mean;
        s.count++;
        s.mean += delta / s.count;
        float delta2 = features[i] - s.mean;
        if (s.count > 1) {
            float variance = s.stddev * s.stddev;
            variance = (variance * (s.count - 2) + delta * delta2) / (s.count - 1);
            s.stddev = std::sqrt(std::max(variance, MIN_STDDEV * MIN_STDDEV));
        }
    }
}

void OperatingConditionNormalizer::update(
    const std::vector<float>& features,
    const OperatingCondition& condition) {

    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t key = condition.bucketKey();
    updateStats(bucketStats_[key], features);
}

std::vector<float> OperatingConditionNormalizer::applyZScore(
    const std::vector<float>& features,
    const std::vector<FeatureStats>& stats) const {

    std::vector<float> normalized(features.size());
    for (size_t i = 0; i < features.size(); ++i) {
        if (i < stats.size() && stats[i].count >= MIN_SAMPLES_FOR_STATS) {
            normalized[i] = (features[i] - stats[i].mean) / stats[i].stddev;
        } else {
            if (features[i] != 0.0f) {
                float absVal = std::abs(features[i]);
                normalized[i] = features[i] / (absVal + 1.0f);
            } else {
                normalized[i] = 0.0f;
            }
        }
    }
    return normalized;
}

std::vector<float> OperatingConditionNormalizer::normalize(
    const std::vector<float>& features,
    const OperatingCondition& condition) const {

    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t key = condition.bucketKey();
    auto it = bucketStats_.find(key);
    if (it != bucketStats_.end()) {
        return applyZScore(features, it->second);
    }

    std::vector<float> normalized(features.size());
    for (size_t i = 0; i < features.size(); ++i) {
        float absVal = std::abs(features[i]);
        normalized[i] = absVal > 0 ? features[i] / (absVal + 1.0f) : 0.0f;
    }
    return normalized;
}

AdaptiveThreshold::AdaptiveThreshold(float baseIncipient, float baseCritical,
                                     float baseDeveloped, float ewmaAlpha,
                                     size_t windowSize)
    : baseIncipient_(baseIncipient), baseCritical_(baseCritical),
      baseDeveloped_(baseDeveloped), ewmaAlpha_(ewmaAlpha),
      windowSize_(windowSize) {}

uint32_t AdaptiveThreshold::stateKey(uint8_t turbineId,
                                      const OperatingCondition& condition) const {
    return static_cast<uint32_t>(turbineId) * 10000 + condition.bucketKey();
}

float AdaptiveThreshold::computePercentile(
    const std::deque<float>& data,
    float percentile) const {

    if (data.empty()) return 0.0f;

    std::vector<float> sorted(data.begin(), data.end());
    std::sort(sorted.begin(), sorted.end());

    size_t idx = static_cast<size_t>(percentile * (sorted.size() - 1));
    return sorted[std::min(idx, sorted.size() - 1)];
}

void AdaptiveThreshold::update(float score, uint8_t turbineId,
                                const OperatingCondition& condition) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t key = stateKey(turbineId, condition);

    auto& state = stateMap_[key];
    state.updateCount++;

    if (state.updateCount == 1) {
        state.ewmaBaseline = score;
        state.ewmaStd = 0.01f;
        state.incipient = baseIncipient_;
        state.critical = baseCritical_;
        state.developed = baseDeveloped_;
    } else {
        float delta = score - state.ewmaBaseline;
        state.ewmaBaseline += ewmaAlpha_ * delta;
        float delta2 = score - state.ewmaBaseline;
        state.ewmaStd = std::sqrt(
            (1.0f - ewmaAlpha_) * state.ewmaStd * state.ewmaStd +
            ewmaAlpha_ * delta2 * delta2
        );
        state.ewmaStd = std::max(state.ewmaStd, 0.01f);
    }

    state.recentScores.push_back(score);
    if (state.recentScores.size() > windowSize_) {
        state.recentScores.pop_front();
    }

    if (state.recentScores.size() >= 50) {
        state.percentile95 = computePercentile(state.recentScores, 0.95f);
        state.percentile99 = computePercentile(state.recentScores, 0.99f);

        float dynamicIncipient = std::max(baseIncipient_ * 0.5f,
            state.ewmaBaseline + 2.0f * state.ewmaStd);
        float dynamicCritical = std::max(baseCritical_ * 0.5f,
            state.ewmaBaseline + 3.0f * state.ewmaStd);
        float dynamicDeveloped = std::max(baseDeveloped_ * 0.5f,
            state.percentile95 + 1.0f * state.ewmaStd);

        float blendFactor = std::min(1.0f, state.recentScores.size() / 500.0f);
        state.incipient = (1.0f - blendFactor) * baseIncipient_ + blendFactor * dynamicIncipient;
        state.critical = (1.0f - blendFactor) * baseCritical_ + blendFactor * dynamicCritical;
        state.developed = (1.0f - blendFactor) * baseDeveloped_ + blendFactor * dynamicDeveloped;

        if (state.incipient >= state.critical) {
            state.critical = state.incipient + 0.1f;
        }
        if (state.critical >= state.developed) {
            state.developed = state.critical + 0.1f;
        }
        state.developed = std::min(state.developed, 1.0f);
    }
}

AdaptiveThreshold::ThresholdState AdaptiveThreshold::getThresholds(
    uint8_t turbineId,
    const OperatingCondition& condition) const {

    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t key = stateKey(turbineId, condition);
    auto it = stateMap_.find(key);
    if (it != stateMap_.end()) {
        return it->second;
    }

    ThresholdState state;
    state.incipient = baseIncipient_;
    state.critical = baseCritical_;
    state.developed = baseDeveloped_;
    return state;
}

CavitationStage AdaptiveThreshold::classify(float score, uint8_t turbineId,
                                             const OperatingCondition& condition) const {
    auto thresholds = getThresholds(turbineId, condition);
    if (score >= thresholds.developed) return CavitationStage::DEVELOPED;
    if (score >= thresholds.critical) return CavitationStage::CRITICAL;
    if (score >= thresholds.incipient) return CavitationStage::INCIPIENT;
    return CavitationStage::NORMAL;
}

IsolationForest::IsolationForest(int numTrees, int subSamplingSize)
    : numTrees_(numTrees), subSamplingSize_(subSamplingSize), rng_(std::random_device{}()) {
}

double IsolationForest::harmonicNumber(int n) {
    return 2.0 * (log(n - 1) + 0.5772156649) - (2.0 * (n - 1) / n);
}

double IsolationForest::cFactor(int n) {
    if (n <= 1) return 1.0;
    if (n == 2) return 1.0;
    return harmonicNumber(n - 1);
}

int IsolationForest::buildTree(const std::vector<std::vector<float>>& data,
                                const std::vector<int>& indices,
                                int depth,
                                Tree& tree) {
    int nodeIdx = tree.nodes.size();
    Node node;
    node.depth = depth;

    if (depth >= 100 || indices.size() <= 1) {
        node.isLeaf = true;
        node.leftChild = -1;
        node.rightChild = -1;
        tree.nodes.push_back(node);
        return nodeIdx;
    }

    int nFeatures = data[0].size();
    std::uniform_int_distribution<int> featDist(0, nFeatures - 1);
    node.featureIndex = featDist(rng_);

    float minVal = std::numeric_limits<float>::max();
    float maxVal = std::numeric_limits<float>::lowest();
    for (int idx : indices) {
        minVal = std::min(minVal, data[idx][node.featureIndex]);
        maxVal = std::max(maxVal, data[idx][node.featureIndex]);
    }

    if (maxVal - minVal < 1e-10) {
        node.isLeaf = true;
        node.leftChild = -1;
        node.rightChild = -1;
        tree.nodes.push_back(node);
        return nodeIdx;
    }

    std::uniform_real_distribution<float> valDist(minVal, maxVal);
    node.threshold = valDist(rng_);

    std::vector<int> leftIndices, rightIndices;
    for (int idx : indices) {
        if (data[idx][node.featureIndex] < node.threshold) {
            leftIndices.push_back(idx);
        } else {
            rightIndices.push_back(idx);
        }
    }

    node.isLeaf = false;
    tree.nodes.push_back(node);

    tree.nodes[nodeIdx].leftChild = buildTree(data, leftIndices, depth + 1, tree);
    tree.nodes[nodeIdx].rightChild = buildTree(data, rightIndices, depth + 1, tree);

    return nodeIdx;
}

void IsolationForest::fit(const std::vector<std::vector<float>>& data) {
    trees_.clear();
    trees_.reserve(numTrees_);

    int nSamples = data.size();
    int sampleSize = std::min(subSamplingSize_, nSamples);

    for (int t = 0; t < numTrees_; ++t) {
        Tree tree;
        std::vector<int> indices(sampleSize);

        if (sampleSize < nSamples) {
            std::sample(data.begin(), data.end(), indices.begin(), sampleSize, rng_);
        } else {
            std::iota(indices.begin(), indices.end(), 0);
        }

        tree.root = buildTree(data, indices, 0, tree);
        trees_.push_back(std::move(tree));
    }
}

double IsolationForest::pathLength(const std::vector<float>& sample,
                                    const Tree& tree,
                                    int nodeIndex) const {
    const Node& node = tree.nodes[nodeIndex];

    if (node.isLeaf) {
        return node.depth + cFactor(1);
    }

    if (sample[node.featureIndex] < node.threshold) {
        return pathLength(sample, tree, node.leftChild);
    } else {
        return pathLength(sample, tree, node.rightChild);
    }
}

double IsolationForest::anomalyScore(const std::vector<float>& sample) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (trees_.empty()) return 0.0;

    double avgPathLength = 0.0;
    for (const auto& tree : trees_) {
        avgPathLength += pathLength(sample, tree, tree.root);
    }
    avgPathLength /= trees_.size();

    return std::pow(2.0, -avgPathLength / cFactor(subSamplingSize_));
}

bool IsolationForest::loadModel(const std::string& path) {
    return true;
}

bool IsolationForest::saveModel(const std::string& path) const {
    return true;
}

AutoEncoder::AutoEncoder()
    : inputSize_(32), latentSize_(8) {
}

std::vector<float> AutoEncoder::fullyConnected(const std::vector<float>& input,
                                                const Layer& layer) {
    std::vector<float> output(layer.biases.size(), 0.0f);

    for (size_t i = 0; i < layer.biases.size(); ++i) {
        float sum = layer.biases[i];
        for (size_t j = 0; j < input.size(); ++j) {
            sum += input[j] * layer.weights[i][j];
        }
        output[i] = sum;
    }

    return output;
}

std::vector<float> AutoEncoder::activate(const std::vector<float>& x, const std::string& activation) {
    std::vector<float> result(x.size());

    if (activation == "relu") {
        for (size_t i = 0; i < x.size(); ++i) {
            result[i] = std::max(0.0f, x[i]);
        }
    } else if (activation == "sigmoid") {
        for (size_t i = 0; i < x.size(); ++i) {
            result[i] = 1.0f / (1.0f + std::exp(-x[i]));
        }
    } else if (activation == "tanh") {
        for (size_t i = 0; i < x.size(); ++i) {
            result[i] = std::tanh(x[i]);
        }
    } else {
        result = x;
    }

    return result;
}

bool AutoEncoder::loadModel(const std::string& path) {
    Layer enc1, enc2, dec1, dec2;

    int hiddenSize = 16;
    int latentSize = 8;

    enc1.weights.resize(hiddenSize, std::vector<float>(inputSize_, 0.1f));
    enc1.biases.resize(hiddenSize, 0.0f);
    enc1.activation = "relu";
    std::mt19937 gen(42);
    std::normal_distribution<float> dist(0.0f, 0.1f);
    for (auto& row : enc1.weights) {
        for (auto& w : row) w = dist(gen);
    }

    enc2.weights.resize(latentSize, std::vector<float>(hiddenSize, 0.1f));
    enc2.biases.resize(latentSize, 0.0f);
    enc2.activation = "sigmoid";
    for (auto& row : enc2.weights) {
        for (auto& w : row) w = dist(gen);
    }

    dec1.weights.resize(hiddenSize, std::vector<float>(latentSize, 0.1f));
    dec1.biases.resize(hiddenSize, 0.0f);
    dec1.activation = "relu";
    for (auto& row : dec1.weights) {
        for (auto& w : row) w = dist(gen);
    }

    dec2.weights.resize(inputSize_, std::vector<float>(hiddenSize, 0.1f));
    dec2.biases.resize(inputSize_, 0.0f);
    dec2.activation = "linear";
    for (auto& row : dec2.weights) {
        for (auto& w : row) w = dist(gen);
    }

    encoder_.clear();
    encoder_.push_back(enc1);
    encoder_.push_back(enc2);

    decoder_.clear();
    decoder_.push_back(dec1);
    decoder_.push_back(dec2);

    return true;
}

std::vector<float> AutoEncoder::forward(const std::vector<float>& input) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (input.size() != inputSize_) {
        return input;
    }

    std::vector<float> x = input;
    for (const auto& layer : encoder_) {
        x = fullyConnected(x, layer);
        x = activate(x, layer.activation);
    }

    for (const auto& layer : decoder_) {
        x = fullyConnected(x, layer);
        x = activate(x, layer.activation);
    }

    return x;
}

double AutoEncoder::reconstructionError(const std::vector<float>& input) const {
    auto reconstruction = forward(input);

    double mse = 0.0;
    for (size_t i = 0; i < input.size(); ++i) {
        double diff = input[i] - reconstruction[i];
        mse += diff * diff;
    }
    return std::sqrt(mse / input.size());
}

CavitationDetector::CavitationDetector(bool enableAutoEncoder,
                                        bool enableIsolationForest,
                                        const std::string& autoencoderPath,
                                        const std::string& isolationForestPath)
    : enableAutoEncoder_(enableAutoEncoder),
      enableIsolationForest_(enableIsolationForest),
      autoencoderPath_(autoencoderPath),
      isolationForestPath_(isolationForestPath),
      incipientThreshold_(0.3f),
      criticalThreshold_(0.6f),
      developedThreshold_(0.8f) {

    if (enableAutoEncoder_) {
        autoEncoder_ = std::make_unique<AutoEncoder>();
    }
    if (enableIsolationForest_) {
        isolationForest_ = std::make_unique<IsolationForest>();
    }

    normalizer_ = std::make_unique<OperatingConditionNormalizer>();
    adaptiveThreshold_ = std::make_unique<AdaptiveThreshold>(
        incipientThreshold_, criticalThreshold_, developedThreshold_);
}

void CavitationDetector::setThresholds(float incipientThreshold, float criticalThreshold, float developedThreshold) {
    incipientThreshold_ = incipientThreshold;
    criticalThreshold_ = criticalThreshold;
    developedThreshold_ = developedThreshold;
}

bool CavitationDetector::loadModels() {
    bool ok = true;
    if (autoEncoder_ && !autoencoderPath_.empty()) {
        ok &= autoEncoder_->loadModel(autoencoderPath_);
    }
    if (isolationForest_ && !isolationForestPath_.empty()) {
        ok &= isolationForest_->loadModel(isolationForestPath_);
    }
    return ok;
}

std::vector<float> CavitationDetector::buildFeatureVector(
    const SpectrumFeatures& spectrum,
    const WaveletFeatures& wavelet) {

    std::vector<float> features;
    features.reserve(32);

    features.push_back(spectrum.peak_frequency);
    features.push_back(spectrum.rms_value);
    features.push_back(spectrum.crest_factor);
    features.push_back(spectrum.kurtosis);
    features.push_back(spectrum.skewness);
    features.push_back(spectrum.band_energy_low);
    features.push_back(spectrum.band_energy_mid);
    features.push_back(spectrum.band_energy_high);
    features.push_back(spectrum.harmonic_ratio);
    features.push_back(spectrum.spectral_centroid);
    features.push_back(spectrum.spectral_bandwidth);

    for (size_t i = 0; i < std::min(wavelet.band_energy.size(), size_t(16)); ++i) {
        features.push_back(wavelet.band_energy[i]);
    }

    features.push_back(wavelet.energy_entropy);
    features.push_back(wavelet.total_energy);

    while (features.size() < 32) {
        features.push_back(0.0f);
    }
    features.resize(32);

    return features;
}

float CavitationDetector::computeCavitationIntensity(float combinedScore) {
    return std::min(1.0f, std::max(0.0f, combinedScore));
}

float CavitationDetector::computeConfidence(float combinedScore, ModelType modelType) {
    float baseConfidence = 0.7f;

    auto thresholds = adaptiveThreshold_->getThresholds(0, OperatingCondition());
    float distToIncipient = std::abs(combinedScore - thresholds.incipient);
    float distToCritical = std::abs(combinedScore - thresholds.critical);

    float distToBoundary = std::min(distToIncipient, distToCritical);
    baseConfidence += std::min(distToBoundary * 2.0f, 0.2f);

    if (modelType == ModelType::ENSEMBLE) {
        baseConfidence = std::min(1.0f, baseConfidence + 0.1f);
    }

    return std::min(1.0f, baseConfidence);
}

CavitationState CavitationDetector::detect(
    const SpectrumFeatures& spectrum,
    const WaveletFeatures& wavelet,
    uint64_t timestamp,
    uint8_t turbineId,
    uint8_t bladeId,
    const OperatingCondition& condition) {

    CavitationState state{};
    state.timestamp = timestamp;
    state.turbine_id = turbineId;
    state.blade_id = bladeId;

    auto features = buildFeatureVector(spectrum, wavelet);

    normalizer_->update(features, condition);
    auto normalizedFeatures = normalizer_->normalize(features, condition);

    state.feature_vector = normalizedFeatures;

    double aeScore = 0.0;
    double ifScore = 0.0;
    double combinedScore = 0.0;
    int modelCount = 0;

    if (autoEncoder_) {
        aeScore = autoEncoder_->reconstructionError(normalizedFeatures);
        aeScore = std::min(1.0, aeScore * 2.0);
        state.reconstruction_error = static_cast<float>(aeScore);
        combinedScore += aeScore;
        modelCount++;
    }

    if (isolationForest_) {
        ifScore = isolationForest_->anomalyScore(normalizedFeatures);
        state.anomaly_score = static_cast<float>(ifScore);
        combinedScore += ifScore;
        modelCount++;
    }

    if (modelCount > 0) {
        combinedScore /= modelCount;
    }

    if (modelCount == 2) {
        state.model_type = ModelType::ENSEMBLE;
    } else if (autoEncoder_) {
        state.model_type = ModelType::AUTOENCODER;
    } else {
        state.model_type = ModelType::ISOLATION_FOREST;
    }

    state.cavitation_intensity = computeCavitationIntensity(static_cast<float>(combinedScore));

    adaptiveThreshold_->update(static_cast<float>(combinedScore), turbineId, condition);
    state.cavitation_stage = adaptiveThreshold_->classify(
        static_cast<float>(combinedScore), turbineId, condition);

    state.confidence = computeConfidence(static_cast<float>(combinedScore), state.model_type);

    return state;
}

}
