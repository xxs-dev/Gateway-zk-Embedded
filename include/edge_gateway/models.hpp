#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "edge_gateway/compat.hpp"

namespace edge_gateway {

struct CachePolicy {
    bool storeLatest = true;
    bool storeHistory = true;
    std::size_t historySize = 100;
    std::int64_t ttlMs = 600000;
};

struct ReadSpec {
    bool enable = false;
    int function = 3;
    int length = 1;
    std::string dataType;
    double scale = 1.0;
    double offset = 0.0;
    std::string byteOrder = "AB";
    bool signedFlag = false;
    std::string unit;
    int intervalMs = 500;
    int bit = -1;
    std::string dlt645Di;
    int dlt645ByteCount = 0;
    std::string dlt645Decoder;
    CachePolicy cachePolicy;
};

struct WriteSpec {
    bool enable = false;
    int function = 6;
    int length = 1;
    std::string dataType;
    double scale = 1.0;
    double offset = 0.0;
    std::string byteOrder = "AB";
    Optional<double> minValue;
    Optional<double> maxValue;
    double step = 0.0;
    std::vector<double> allowedValues;
    bool verifyAfterWrite = false;
    int verifyDelayMs = 200;
    bool verifyByRead = true;
};

struct AlarmRuleConfig {
    std::string type;
    double threshold = 0.0;
    bool reportRecovery = true;
    std::string persistValue;
};

struct PointDefinition {
    std::uint32_t index = 0;
    std::string pointCode;
    std::string name;
    std::string desc;
    std::string category;
    int address = 0;
    bool enabled = true;
    bool isStore = false;
    bool fullUpload = false;
    bool reportOnChange = false;
    int persistIntervalSec = 60;
    std::vector<std::string> tags;
    ReadSpec read;
    WriteSpec write;
    std::vector<AlarmRuleConfig> alarms;
    std::unordered_map<std::string, std::string> valueMap;
};

struct PointValue {
    std::uint32_t index = 0;
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    std::string pointName;
    std::string category;
    std::string unit;
    double value = 0.0;
    std::string text;
    std::string rawHex;
    int quality = 1;
    std::string qualityMsg = "ok";
    std::int64_t ts = 0;
    std::int64_t expireAt = 0;
    bool stale = false;
    int function = 3;
    int address = 0;
    int length = 1;
    bool isStore = false;
    int persistIntervalSec = 60;
};

struct StoredPointValue {
    std::uint32_t index = 0;
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    double value = 0.0;
    int quality = 1;
    std::int64_t ts = 0;
    std::int64_t expireAt = 0;
    bool stale = false;
};

struct PointLeaseStatus {
    std::uint32_t index = 0;
    std::size_t ownerCount = 0;
    bool hasActiveOwner = false;
    std::int64_t lastClaimTs = 0;
};

struct AlarmEvent {
    std::uint32_t index = 0;
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    std::string alarmType;
    bool active = false;
    double threshold = 0.0;
    double value = 0.0;
    int quality = 1;
    std::int64_t ts = 0;
    bool stale = false;
    std::string persistValue;
};

struct PersistentPointSample {
    std::uint32_t index = 0;
    double value = 0.0;
    std::int64_t ts = 0;
};

struct PointUpdateRecord {
    std::uint64_t sequence = 0;
    std::uint32_t index = 0;
    double value = 0.0;
    int quality = 1;
    std::int64_t ts = 0;
    std::int64_t expireAt = 0;
};

struct PendingWriteCommand {
    std::string cmdId;
    std::uint32_t index = 0;
    double value = 0.0;
    std::string source;
    std::int64_t ts = 0;
};

struct MqttCommandRequest {
    std::string cmdId;
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    std::uint32_t index = 0;
    double value = 0.0;
    std::string source = "mqtt";
    std::int64_t ts = 0;
};

struct MqttCommandReply {
    std::string cmdId;
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    std::uint32_t index = 0;
    bool success = false;
    std::string message;
    std::int64_t ts = 0;
};

struct OtaRequest {
    std::string jobId;
    std::string machineCode;
    std::string artifactUrl;
    std::string version;
    std::string sha256;
    std::uint64_t size = 0;
    std::string upgradeMode;
    std::int64_t ts = 0;
};

struct OtaReply {
    std::string jobId;
    std::string machineCode;
    bool accepted = false;
    std::string message;
    std::int64_t ts = 0;
};

struct OtaStatus {
    std::string jobId;
    std::string machineCode;
    std::string stage;
    int progress = 0;
    std::string message;
    std::int64_t ts = 0;
};

enum class MqttIncomingType {
    CommandRequest,
    OtaRequest,
    SystemMonitorRequest,
    DiagRequest,
    ConfigPullRequest
};

struct MqttIncomingMessage {
    MqttIncomingType type = MqttIncomingType::CommandRequest;
    std::string topic;
    std::string payload;
};

struct PointBinding {
    std::uint32_t index = 0;
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
};

struct SerialTransportConfig {
    std::string serialPort;
    int baudRate = 9600;
    int dataBits = 8;
    int stopBits = 1;
    std::string parity = "N";
    int timeoutMs = 1000;
};

struct TcpTransportConfig {
    std::string host = "127.0.0.1";
    int port = 502;
    int connectTimeoutMs = 1000;
    int timeoutMs = 1000;
};

struct ProtocolConfig {
    std::string type = "modbus_rtu";
    int slave = 1;
    SerialTransportConfig transport;
    TcpTransportConfig tcp;
    std::string standardPointsFile;
    std::string standardPointsVersion;
};

struct LogicalDeviceConfig {
    std::string meterCode;
    std::string deviceName;
    int slave = 1;
    std::string address;
    std::vector<PointDefinition> points;
};

struct CollectConfig {
    int defaultIntervalMs = 500;
    bool batchOptimize = true;
    int maxBatchRegisters = 120;
};

struct MemoryStoreConfig {
    bool enabled = true;
    std::string backend = "memory";
    std::size_t keepHistory = 100;
    std::int64_t defaultTtlMs = 600000;
    std::vector<std::string> indexBy = {"machineCode", "meterCode", "pointCode"};
    std::string sharedMemoryName = "gateway_point_store";
    std::size_t maxLatestPoints = 100000;
    std::size_t maxPendingWrites = 4096;
    std::size_t maxPersistentSamples = 20000;
    std::string sqlitePath = "point_samples.db";
    std::string sqliteLibraryPath;
    int persistFlushIntervalMs = 60000;
    int writebackIntervalMs = 500;
    std::size_t writebackBatchSize = 100;
};

struct MqttConfig {
    bool enabled = false;
    std::string protocolVersion = "mqtt3";
    std::string broker = "tcp://127.0.0.1:1883";
    std::string clientId = "modbus-gateway";
    std::string topicMachineCode;
    std::string username;
    std::string password;
    std::string telemetryTopic = "edge/telemetry";
    std::string changeEventTopic = "edge/event/change";
    std::string alarmTopic = "edge/alarm";
    std::string statusTopic = "edge/status";
    std::string commandRequestTopic = "edge/command/request";
    std::string commandReplyTopic = "edge/command/reply";
    std::string otaRequestTopic = "edge/ota/request";
    std::string otaReplyTopic = "edge/ota/reply";
    std::string otaStatusTopic = "edge/ota/status";
    std::string systemMonitorRequestTopic = "edge/system/monitor/request";
    std::string systemMonitorReplyTopic = "edge/system/monitor/reply";
    std::string systemMonitorTelemetryTopic = "edge/system/monitor/telemetry";
    std::string systemMonitorAlertTopic = "edge/system/monitor/alert";
    std::string systemMonitorPointTopic = "edge/system/monitor/points";
    std::string diagRequestTopic = "edge/system/diag/request";
    std::string diagReplyTopic = "edge/system/diag/reply";
    std::string configPullRequestTopic = "edge/config/pull/request";
    std::string configPullReplyTopic = "edge/config/pull/reply";
    int qos = 1;
    bool cleanSession = true;
    int keepAliveSec = 60;
    int sessionExpirySec = 0;
    bool offlineBufferEnabled = true;
    std::string offlineBufferMode = "ring";
    std::string offlineBufferDir = "/opt/modbus-gateway/data/mqtt-spool";
    std::string offlineRealtimeFile = "/opt/modbus-gateway/data/mqtt-spool/realtime_ring.dat";
    std::uint64_t offlineRealtimeFileSizeBytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint32_t offlineMaxRealtimeMessageBytes = 4 * 1024 * 1024;
    std::size_t offlineBufferMaxMemoryMessages = 200;
    std::size_t offlineBufferFlushBatchSize = 50;
    int offlineBufferFlushIntervalMs = 5000;
    std::size_t offlineBufferReplayBatchSize = 100;
    std::size_t offlineBufferMaxDiskBytes = 32 * 1024 * 1024;
    std::string eventOutboxSqlitePath = "/opt/modbus-gateway/data/mqtt_event_outbox.db";
    std::string eventOutboxSqliteLibraryPath;
    int eventOutboxRetentionMonths = 12;
    int eventOutboxCleanupIntervalHours = 24;
    std::size_t eventOutboxReplayBatchSize = 100;
    std::size_t maxPayloadBytes = 49152;
};

struct MqttAlarmRule {
    std::uint32_t index = 0;
    Optional<double> high;
    Optional<double> low;
    bool reportRecovery = true;
};

struct MqttDriverConfig {
    bool enabled = false;
    std::string sharedMemoryName = "gateway_point_store";
    std::vector<std::string> sharedMemoryNames;
    int scanIntervalMs = 1000;
    int fullUploadIntervalMs = 60000;
    std::size_t snapshotBacklogThreshold = 0;
    int snapshotBackoffIntervalMs = 0;
    std::size_t eventReplayMaxBytes = 256 * 1024;
    bool publishFullOnStart = true;
    bool publishAllOnFull = true;
    std::vector<std::uint32_t> fullUploadIndexes;
    std::vector<MqttAlarmRule> alarmRules;
};

struct AlarmStoreConfig {
    bool enabled = false;
    std::string sqlitePath = "alarm_events.db";
    std::string sqliteLibraryPath;
};

struct EventEngineConfig {
    bool enabled = false;
    int scanIntervalMs = 100;
    int scanFallbackIntervalMs = 5000;
    std::size_t updateDrainBatchSize = 4096;
    std::string publishMode = "direct_mqtt";
    std::string mqttClientIdSuffix = "event_engine";
};

struct OtaStorageMinioConfig {
    std::string endpoint;
    std::string accessKey;
    std::string secretKey;
    std::string bucket;
    std::string basePath;
    std::string publicBaseUrl;
};

struct OtaStorageConfig {
    std::string provider = "local";
    int presignExpireMinutes = 60;
    OtaStorageMinioConfig minio;
};

struct OtaConfig {
    bool enabled = false;
    std::string currentVersion = "1.0.0";
    std::string artifactBaseUrl;
    std::string downloadDir = "/opt/modbus-gateway/ota/downloads";
    std::string stagingDir = "/opt/modbus-gateway/ota/staging";
    std::string backupDir = "/opt/modbus-gateway/ota/backup";
    std::string packageType = "tar.gz";
    std::string applyScript = "/opt/modbus-gateway/bin/ota-apply.sh";
    std::string rollbackScript = "/opt/modbus-gateway/bin/ota-rollback.sh";
    bool checksumRequired = true;
    bool autoReboot = true;
    int retentionCount = 3;
    int statusReportIntervalSec = 5;
    int upgradeTimeoutSec = 900;
    OtaStorageConfig storage;
};

struct RealtimeConfig {
    bool enabled = false;
    std::string telemetryTopic = "edge/telemetry";
    std::string alarmTopic = "edge/alarm";
    std::string statusTopic = "edge/status";
    std::size_t maxLatestPoints = 100000;
    std::size_t trendBufferSize = 300;
    int pushThrottleMs = 200;
};

struct SystemMonitorConfig {
    bool enabled = false;
    int defaultIntervalMs = 5000;
    int minIntervalMs = 1000;
    int subscriptionTtlSec = 30;
    double cpuAlertThreshold = 90.0;
    double memAlertThreshold = 90.0;
    double diskAlertThreshold = 90.0;
    int alertRepeatIntervalSec = 60;
    bool diagEnabled = true;
    std::vector<std::string> allowedCommands = {
        "uptime",
        "free_m",
        "df_root",
        "top_once",
        "ps_gateway",
        "journal_tail",
        "systemctl_status"
    };
};

struct AppConfig {
    std::vector<std::string> deviceConfigFiles;
    MqttConfig mqtt;
    MqttDriverConfig mqttDriver;
    AlarmStoreConfig alarmStore;
    EventEngineConfig eventEngine;
    OtaConfig ota;
    RealtimeConfig realtime;
    SystemMonitorConfig systemMonitor;
};

struct DeviceConfig {
    std::string schemaVersion = "1.0.0";
    std::string machineCode;
    std::string meterCode;
    std::string deviceName;
    std::string address;
    ProtocolConfig protocol;
    CollectConfig collect;
    MemoryStoreConfig memoryStore;
    std::vector<PointDefinition> points;
    std::vector<LogicalDeviceConfig> meters;
};

struct ReadTaskPoint {
    PointDefinition definition;
    int offset = 0;
};

struct ReadTask {
    int function = 3;
    int start = 0;
    int count = 0;
    std::vector<ReadTaskPoint> points;
};

struct DecodedValue {
    double value = 0.0;
    std::string text;
    std::string rawHex;
};

struct WriteValidationResult {
    bool ok = true;
    std::string message = "ok";
};

struct CommandRequest {
    std::string cmdId;
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    double value = 0.0;
};

struct CommandResult {
    std::string cmdId;
    std::string machineCode;
    std::string meterCode;
    std::string pointCode;
    bool success = false;
    std::string message;
    std::int64_t ts = 0;
    std::uint32_t index = 0;
    double requestedValue = 0.0;
    bool verifyAttempted = false;
    bool verifyPassed = false;
};

struct CommandExecutionException {
    std::string message;
};

}  // namespace edge_gateway
