#include "api_server.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <json/json.h>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace turbine_monitor {

APIServer::APIServer(const std::string& host, uint16_t port,
                     std::shared_ptr<DataProvider> dataProvider)
    : host_(host), port_(port), running_(false),
      serverSocket_(INVALID_SOCKET),
      dataProvider_(std::move(dataProvider)) {
}

APIServer::~APIServer() {
    stop();
}

bool APIServer::isRunning() const {
    return running_;
}

void APIServer::registerHandler(const std::string& path, ApiHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handlers_[path] = std::move(handler);
}

std::string APIServer::jsonError(const std::string& message, int code) {
    Json::Value json;
    json["error"]["code"] = code;
    json["error"]["message"] = message;
    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, json);
}

std::string APIServer::cavitationStateToJson(const CavitationState& state) {
    Json::Value json;
    json["timestamp"] = Json::Value::UInt64(state.timestamp);
    json["turbine_id"] = state.turbine_id;
    json["blade_id"] = state.blade_id;
    json["cavitation_stage"] = static_cast<int>(state.cavitation_stage);
    json["cavitation_intensity"] = state.cavitation_intensity;
    json["confidence"] = state.confidence;
    json["model_type"] = static_cast<int>(state.model_type);
    json["anomaly_score"] = state.anomaly_score;
    json["reconstruction_error"] = state.reconstruction_error;

    Json::Value features(Json::arrayValue);
    for (float f : state.feature_vector) {
        features.append(f);
    }
    json["feature_vector"] = features;

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, json);
}

std::string APIServer::lifeAssessmentToJson(const LifeAssessment& a) {
    Json::Value json;
    json["timestamp"] = Json::Value::UInt64(a.timestamp);
    json["turbine_id"] = a.turbine_id;
    json["blade_id"] = a.blade_id;
    json["cumulative_damage"] = a.cumulative_damage;
    json["remaining_life_hours"] = a.remaining_life_hours;
    json["remaining_life_days"] = a.remaining_life_days;
    json["miner_sum"] = a.miner_sum;
    json["fatigue_damage"] = a.fatigue_damage;
    json["cavitation_damage"] = a.cavitation_damage;
    json["material_constant_k"] = a.material_constant_k;
    json["material_constant_m"] = a.material_constant_m;
    json["stress_range"] = a.stress_range;
    json["cycle_count"] = a.cycle_count;
    json["assessment_method"] = a.assessment_method;

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, json);
}

std::string APIServer::spectrumToJson(const SpectrumFeatures& f) {
    Json::Value json;
    json["timestamp"] = Json::Value::UInt64(f.timestamp);
    json["turbine_id"] = f.turbine_id;
    json["sensor_type"] = static_cast<int>(f.sensor_type);
    json["sensor_id"] = f.sensor_id;
    json["blade_id"] = f.blade_id;
    json["peak_frequency"] = f.peak_frequency;
    json["rms_value"] = f.rms_value;
    json["crest_factor"] = f.crest_factor;
    json["kurtosis"] = f.kurtosis;
    json["skewness"] = f.skewness;
    json["band_energy_low"] = f.band_energy_low;
    json["band_energy_mid"] = f.band_energy_mid;
    json["band_energy_high"] = f.band_energy_high;
    json["harmonic_ratio"] = f.harmonic_ratio;
    json["spectral_centroid"] = f.spectral_centroid;
    json["spectral_bandwidth"] = f.spectral_bandwidth;

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, json);
}

std::string APIServer::alarmToJson(const AlarmLog& a) {
    Json::Value json;
    json["timestamp"] = Json::Value::UInt64(a.timestamp);
    json["alarm_id"] = a.alarm_id;
    json["turbine_id"] = a.turbine_id;
    json["blade_id"] = a.blade_id;
    json["alarm_type"] = static_cast<int>(a.alarm_type);
    json["alarm_level"] = static_cast<int>(a.alarm_level);
    json["alarm_message"] = a.alarm_message;
    json["threshold_value"] = a.threshold_value;
    json["actual_value"] = a.actual_value;
    json["iec61850_pushed"] = a.iec61850_pushed;
    json["acknowledged"] = a.acknowledged;
    json["maintenance_suggestion"] = a.maintenance_suggestion;
    json["acknowledged_at"] = Json::Value::UInt64(a.acknowledged_at);
    json["acknowledged_by"] = a.acknowledged_by;

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, json);
}

std::string APIServer::turbineConfigToJson(const TurbineConfig& c) {
    Json::Value json;
    json["turbine_id"] = c.turbine_id;
    json["turbine_name"] = c.turbine_name;
    json["turbine_type"] = c.turbine_type;
    json["rated_power"] = c.rated_power;
    json["rated_head"] = c.rated_head;
    json["rated_flow"] = c.rated_flow;
    json["rated_speed"] = c.rated_speed;
    json["blade_count"] = c.blade_count;
    json["material"] = c.material;
    json["ultimate_tensile_strength"] = c.ultimate_tensile_strength;
    json["fatigue_limit"] = c.fatigue_limit;
    json["fracture_toughness"] = c.fracture_toughness;
    json["cavitation_threshold"] = c.cavitation_threshold;
    json["vibration_threshold"] = c.vibration_threshold;
    json["expected_life_hours"] = c.expected_life_hours;

    Json::StreamWriterBuilder writer;
    return Json::writeString(writer, json);
}

HttpResponse APIServer::getTurbineList(const HttpRequest& req) {
    auto configs = dataProvider_->getTurbineConfigs();
    Json::Value json(Json::arrayValue);
    for (const auto& c : configs) {
        Json::Value item;
        item["turbine_id"] = c.turbine_id;
        item["turbine_name"] = c.turbine_name;
        item["turbine_type"] = c.turbine_type;
        item["rated_power"] = c.rated_power;
        json.append(item);
    }
    Json::StreamWriterBuilder writer;
    return {200, "application/json", Json::writeString(writer, json), {}};
}

HttpResponse APIServer::getTurbineDetail(const HttpRequest& req) {
    auto it = req.params.find("id");
    if (it == req.params.end()) {
        return {400, "application/json", jsonError("Missing turbine id", 400), {}};
    }
    uint8_t turbineId = static_cast<uint8_t>(std::stoi(it->second));

    auto configs = dataProvider_->getTurbineConfigs();
    auto configIt = std::find_if(configs.begin(), configs.end(),
        [turbineId](const TurbineConfig& c) { return c.turbine_id == turbineId; });

    if (configIt == configs.end()) {
        return {404, "application/json", jsonError("Turbine not found", 404), {}};
    }

    Json::Value json;
    json["config"] = Json::Value(Json::objectValue);
    json["config"] = Json::parse(turbineConfigToJson(*configIt));

    auto cavitation = dataProvider_->getCavitationState(turbineId);
    Json::Value cavArray(Json::arrayValue);
    for (const auto& c : cavitation) {
        cavArray.append(Json::parse(cavitationStateToJson(c)));
    }
    json["cavitation"] = cavArray;

    auto life = dataProvider_->getLifeAssessment(turbineId, 0);
    Json::Value lifeArray(Json::arrayValue);
    for (const auto& l : life) {
        lifeArray.append(Json::parse(lifeAssessmentToJson(l)));
    }
    json["life_assessment"] = lifeArray;

    auto alarms = dataProvider_->getActiveAlarms(turbineId);
    Json::Value alarmArray(Json::arrayValue);
    for (const auto& a : alarms) {
        alarmArray.append(Json::parse(alarmToJson(a)));
    }
    json["active_alarms"] = alarmArray;

    Json::StreamWriterBuilder writer;
    return {200, "application/json", Json::writeString(writer, json), {}};
}

HttpResponse APIServer::getCavitationState(const HttpRequest& req) {
    auto it = req.params.find("turbine_id");
    if (it == req.params.end()) {
        return {400, "application/json", jsonError("Missing turbine_id", 400), {}};
    }
    uint8_t turbineId = static_cast<uint8_t>(std::stoi(it->second));

    auto states = dataProvider_->getCavitationState(turbineId);

    Json::Value json(Json::arrayValue);
    for (const auto& s : states) {
        json.append(Json::parse(cavitationStateToJson(s)));
    }

    Json::StreamWriterBuilder writer;
    return {200, "application/json", Json::writeString(writer, json), {}};
}

HttpResponse APIServer::getLifeAssessment(const HttpRequest& req) {
    auto turbineIt = req.params.find("turbine_id");
    auto bladeIt = req.params.find("blade_id");
    if (turbineIt == req.params.end()) {
        return {400, "application/json", jsonError("Missing turbine_id", 400), {}};
    }
    uint8_t turbineId = static_cast<uint8_t>(std::stoi(turbineIt->second));
    uint8_t bladeId = bladeIt != req.params.end() ? static_cast<uint8_t>(std::stoi(bladeIt->second)) : 0;

    auto assessments = dataProvider_->getLifeAssessment(turbineId, bladeId);

    Json::Value json(Json::arrayValue);
    for (const auto& a : assessments) {
        json.append(Json::parse(lifeAssessmentToJson(a)));
    }

    Json::StreamWriterBuilder writer;
    return {200, "application/json", Json::writeString(writer, json), {}};
}

HttpResponse APIServer::getSpectrumData(const HttpRequest& req) {
    auto turbineIt = req.params.find("turbine_id");
    auto sensorIt = req.params.find("sensor_id");
    if (turbineIt == req.params.end() || sensorIt == req.params.end()) {
        return {400, "application/json", jsonError("Missing parameters", 400), {}};
    }
    uint8_t turbineId = static_cast<uint8_t>(std::stoi(turbineIt->second));
    uint8_t sensorId = static_cast<uint8_t>(std::stoi(sensorIt->second));

    auto spectra = dataProvider_->getSpectrumData(turbineId, sensorId);

    Json::Value json(Json::arrayValue);
    for (const auto& s : spectra) {
        json.append(Json::parse(spectrumToJson(s)));
    }

    Json::StreamWriterBuilder writer;
    return {200, "application/json", Json::writeString(writer, json), {}};
}

HttpResponse APIServer::getWaterfallData(const HttpRequest& req) {
    auto turbineIt = req.params.find("turbine_id");
    auto sensorIt = req.params.find("sensor_id");
    auto limitIt = req.params.find("limit");

    if (turbineIt == req.params.end() || sensorIt == req.params.end()) {
        return {400, "application/json", jsonError("Missing parameters", 400), {}};
    }

    uint8_t turbineId = static_cast<uint8_t>(std::stoi(turbineIt->second));
    uint8_t sensorId = static_cast<uint8_t>(std::stoi(sensorIt->second));
    int limit = limitIt != req.params.end() ? std::stoi(limitIt->second) : 100;

    auto spectra = dataProvider_->getSpectrumData(turbineId, sensorId);
    if (spectra.size() > static_cast<size_t>(limit)) {
        spectra.resize(limit);
    }

    Json::Value json(Json::arrayValue);
    for (const auto& s : spectra) {
        Json::Value row;
        row["timestamp"] = Json::Value::UInt64(s.timestamp);
        row["rms"] = s.rms_value;
        row["peak_freq"] = s.peak_frequency;
        row["kurtosis"] = s.kurtosis;
        row["crest"] = s.crest_factor;
        row["band_low"] = s.band_energy_low;
        row["band_mid"] = s.band_energy_mid;
        row["band_high"] = s.band_energy_high;
        json.append(row);
    }

    Json::StreamWriterBuilder writer;
    return {200, "application/json", Json::writeString(writer, json), {}};
}

HttpResponse APIServer::getActiveAlarms(const HttpRequest& req) {
    auto it = req.params.find("turbine_id");
    uint8_t turbineId = it != req.params.end() ? static_cast<uint8_t>(std::stoi(it->second)) : 0;

    auto alarms = dataProvider_->getActiveAlarms(turbineId);

    Json::Value json(Json::arrayValue);
    for (const auto& a : alarms) {
        json.append(Json::parse(alarmToJson(a)));
    }

    Json::StreamWriterBuilder writer;
    return {200, "application/json", Json::writeString(writer, json), {}};
}

HttpResponse APIServer::getAlarmHistory(const HttpRequest& req) {
    return {200, "application/json", "[]", {}};
}

HttpResponse APIServer::acknowledgeAlarm(const HttpRequest& req) {
    Json::CharReaderBuilder builder;
    Json::Value body;
    std::string errs;
    std::istringstream ss(req.body);
    if (!Json::parseFromStream(builder, ss, &body, &errs)) {
        return {400, "application/json", jsonError("Invalid JSON body", 400), {}};
    }

    if (!body.isMember("alarm_id") || !body.isMember("user")) {
        return {400, "application/json", jsonError("Missing alarm_id or user", 400), {}};
    }

    std::string alarmId = body["alarm_id"].asString();
    std::string user = body["user"].asString();

    bool success = dataProvider_->acknowledgeAlarm(alarmId, user);

    Json::Value json;
    json["success"] = success;
    json["message"] = success ? "Alarm acknowledged" : "Failed to acknowledge alarm";

    Json::StreamWriterBuilder writer;
    return {success ? 200 : 500, "application/json", Json::writeString(writer, json), {}};
}

HttpResponse APIServer::suppressAlarm(const HttpRequest& req) {
    Json::CharReaderBuilder builder;
    Json::Value body;
    std::string errs;
    std::istringstream ss(req.body);
    if (!Json::parseFromStream(builder, ss, &body, &errs)) {
        return {400, "application/json", jsonError("Invalid JSON body", 400), {}};
    }

    if (!body.isMember("turbine_id") || !body.isMember("blade_id") ||
        !body.isMember("alarm_type") || !body.isMember("duration_ms")) {
        return {400, "application/json", jsonError("Missing parameters", 400), {}};
    }

    uint8_t turbineId = static_cast<uint8_t>(body["turbine_id"].asUInt());
    uint8_t bladeId = static_cast<uint8_t>(body["blade_id"].asUInt());
    AlarmType type = static_cast<AlarmType>(body["alarm_type"].asUInt());
    uint32_t durationMs = body["duration_ms"].asUInt();

    bool success = dataProvider_->suppressAlarm(turbineId, bladeId, type, durationMs);

    Json::Value json;
    json["success"] = success;

    Json::StreamWriterBuilder writer;
    return {success ? 200 : 500, "application/json", Json::writeString(writer, json), {}};
}

HttpResponse APIServer::getSystemStatus(const HttpRequest& req) {
    Json::Value json;
    json["status"] = "running";
    json["timestamp"] = Json::Value::UInt64(currentTimestampMs());

    Json::StreamWriterBuilder writer;
    return {200, "application/json", Json::writeString(writer, json), {}};
}

HttpResponse APIServer::handleRequest(const HttpRequest& request) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = handlers_.find(request.path);
    if (it != handlers_.end()) {
        return it->second(request);
    }

    if (request.path == "/api/turbines") {
        return getTurbineList(request);
    } else if (request.path == "/api/turbine/detail") {
        return getTurbineDetail(request);
    } else if (request.path == "/api/cavitation") {
        return getCavitationState(request);
    } else if (request.path == "/api/life") {
        return getLifeAssessment(request);
    } else if (request.path == "/api/spectrum") {
        return getSpectrumData(request);
    } else if (request.path == "/api/waterfall") {
        return getWaterfallData(request);
    } else if (request.path == "/api/alarms/active") {
        return getActiveAlarms(request);
    } else if (request.path == "/api/alarms/history") {
        return getAlarmHistory(request);
    } else if (request.path == "/api/alarms/acknowledge") {
        return acknowledgeAlarm(request);
    } else if (request.path == "/api/alarms/suppress") {
        return suppressAlarm(request);
    } else if (request.path == "/api/status") {
        return getSystemStatus(request);
    }

    return {404, "application/json", jsonError("Not found", 404), {}};
}

void APIServer::serverLoop() {
    while (running_) {
        sockaddr_in clientAddr{};
#ifdef _WIN32
        int addrLen = sizeof(clientAddr);
#else
        socklen_t addrLen = sizeof(clientAddr);
#endif

        SOCKET clientSocket = accept(serverSocket_, (sockaddr*)&clientAddr, &addrLen);
        if (clientSocket == INVALID_SOCKET) {
            if (running_) {
                std::cerr << "Accept failed" << std::endl;
            }
            continue;
        }

        char buffer[4096];
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer) - 1, 0);
        if (bytesReceived <= 0) {
#ifdef _WIN32
            closesocket(clientSocket);
#else
            close(clientSocket);
#endif
            continue;
        }

        buffer[bytesReceived] = '\0';
        std::string requestStr(buffer);

        HttpRequest request;
        size_t methodEnd = requestStr.find(' ');
        if (methodEnd != std::string::npos) {
            request.method = requestStr.substr(0, methodEnd);
            size_t pathEnd = requestStr.find(' ', methodEnd + 1);
            if (pathEnd != std::string::npos) {
                request.path = requestStr.substr(methodEnd + 1, pathEnd - methodEnd - 1);

                size_t queryStart = request.path.find('?');
                if (queryStart != std::string::npos) {
                    std::string query = request.path.substr(queryStart + 1);
                    request.path = request.path.substr(0, queryStart);

                    size_t pos = 0;
                    while (pos < query.size()) {
                        size_t eq = query.find('=', pos);
                        if (eq == std::string::npos) break;
                        size_t amp = query.find('&', eq);
                        std::string key = query.substr(pos, eq - pos);
                        std::string val = (amp != std::string::npos) ?
                            query.substr(eq + 1, amp - eq - 1) : query.substr(eq + 1);
                        request.params[key] = val;
                        pos = (amp != std::string::npos) ? amp + 1 : query.size();
                    }
                }
            }
        }

        size_t bodyStart = requestStr.find("\r\n\r\n");
        if (bodyStart != std::string::npos) {
            request.body = requestStr.substr(bodyStart + 4);
        }

        HttpResponse response = handleRequest(request);

        std::stringstream responseStream;
        responseStream << "HTTP/1.1 " << response.statusCode << " OK\r\n";
        responseStream << "Content-Type: " << response.contentType << "\r\n";
        responseStream << "Access-Control-Allow-Origin: *\r\n";
        responseStream << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
        responseStream << "Access-Control-Allow-Headers: Content-Type\r\n";
        responseStream << "Content-Length: " << response.body.size() << "\r\n";
        responseStream << "Connection: close\r\n\r\n";
        responseStream << response.body;

        std::string responseStr = responseStream.str();
        send(clientSocket, responseStr.c_str(), responseStr.size(), 0);

#ifdef _WIN32
        closesocket(clientSocket);
#else
        close(clientSocket);
#endif
    }
}

bool APIServer::start() {
#ifdef _WIN32
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        return false;
    }
#endif

    serverSocket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (serverSocket_ == INVALID_SOCKET) {
        return false;
    }

    int opt = 1;
#ifdef _WIN32
    setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, (const char*)&opt, sizeof(opt));
#else
    setsockopt(serverSocket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in serverAddr{};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_port = htons(port_);
    if (host_ == "0.0.0.0" || host_.empty()) {
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    } else {
        inet_pton(AF_INET, host_.c_str(), &serverAddr.sin_addr);
    }

    if (bind(serverSocket_, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::cerr << "Failed to bind API server to " << host_ << ":" << port_ << std::endl;
#ifdef _WIN32
        closesocket(serverSocket_);
        WSACleanup();
#else
        close(serverSocket_);
#endif
        return false;
    }

    if (listen(serverSocket_, 10) == SOCKET_ERROR) {
#ifdef _WIN32
        closesocket(serverSocket_);
        WSACleanup();
#else
        close(serverSocket_);
#endif
        return false;
    }

    running_ = true;
    serverThread_ = std::thread(&APIServer::serverLoop, this);

    std::cout << "API Server started on " << host_ << ":" << port_ << std::endl;
    return true;
}

void APIServer::stop() {
    running_ = false;

    if (serverSocket_ != INVALID_SOCKET) {
#ifdef _WIN32
        closesocket(serverSocket_);
        WSACleanup();
#else
        close(serverSocket_);
#endif
        serverSocket_ = INVALID_SOCKET;
    }

    if (serverThread_.joinable()) {
        serverThread_.join();
    }
}

}
