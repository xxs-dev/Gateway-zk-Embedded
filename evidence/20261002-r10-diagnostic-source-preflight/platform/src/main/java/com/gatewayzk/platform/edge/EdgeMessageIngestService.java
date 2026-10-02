package com.gatewayzk.platform.edge;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.JsonNodeType;
import com.gatewayzk.platform.alarm.AlarmApplicationService;
import com.gatewayzk.platform.configrev.ConfigDeploymentStateService;
import com.gatewayzk.platform.task.DiagnosticResultSanitizer;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Service;
import org.springframework.transaction.annotation.Transactional;
import org.springframework.transaction.support.TransactionSynchronization;
import org.springframework.transaction.support.TransactionSynchronizationManager;

import java.nio.charset.StandardCharsets;
import java.io.ByteArrayOutputStream;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.regex.Pattern;

@Service
public class EdgeMessageIngestService {

    private static final Logger log = LoggerFactory.getLogger(EdgeMessageIngestService.class);
    private static final int MAX_POINT_VALUES = 5_000;
    private static final int MAX_CONFIG_CHUNKS = 2_048;
    private static final long MAX_CONFIG_BYTES = 24L * 1024 * 1024;
    private static final long MAX_CONFIG_RESERVED_BYTES = 128L * 1024 * 1024;
    private static final long CONFIG_CHUNK_TTL_MILLIS = 120_000L;
    private static final Pattern REALTIME_SESSION = Pattern.compile("REALTIME_[A-Za-z0-9-]{1,64}");
    private static final java.util.Set<String> DIAGNOSTIC_REASONS = java.util.Set.of(
        "exit", "signal", "timeout", "cancelled", "launch_failed", "cleanup_unknown",
        "busy", "cache_full", "command_not_allowed", "invalid_command_argument");

    private final EdgeRuntimeRepository repository;
    private final ObjectMapper objectMapper;
    private final AlarmApplicationService alarms;
    private final DiagnosticResultSanitizer diagnosticSanitizer;
    private final ConfigDeploymentStateService deploymentStates;
    private final int maxPayloadBytes;
    // Admission, updates, expiry and completion cleanup share one lock; database work never holds it.
    private final Map<String, ConfigAssembly> configAssemblies = new HashMap<>();

    public EdgeMessageIngestService(EdgeRuntimeRepository repository, ObjectMapper objectMapper,
                                    AlarmApplicationService alarms,
                                    DiagnosticResultSanitizer diagnosticSanitizer,
                                    ConfigDeploymentStateService deploymentStates,
                                    EdgeMqttProperties properties) {
        this.repository = repository;
        this.objectMapper = objectMapper;
        this.alarms = alarms;
        this.diagnosticSanitizer = diagnosticSanitizer;
        this.deploymentStates = deploymentStates;
        this.maxPayloadBytes = properties.maxPayloadBytes();
    }

    @Transactional
    public void ingest(String topic, String payload) {
        if (topic == null || topic.isBlank()) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_TOPIC, "MQTT topic 不能为空");
        }
        int payloadLimit = topic.startsWith("edge/system/diag/reply/")
            ? EdgeMqttProperties.MAX_DIAGNOSTIC_REPLY_BYTES : maxPayloadBytes;
        if (payload == null || payload.getBytes(StandardCharsets.UTF_8).length > payloadLimit) {
            throw rejected(EdgeMessageRejectedException.Reason.PAYLOAD_SIZE, "边端消息超过大小限制");
        }
        JsonNode root;
        try {
            root = topic.startsWith("edge/status/") || topic.startsWith("edge/telemetry/realtime/")
                ? objectMapper.reader().with(com.fasterxml.jackson.databind.DeserializationFeature.FAIL_ON_READING_DUP_TREE_KEY).readTree(payload)
                : objectMapper.readTree(payload);
        } catch (JsonProcessingException exception) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_JSON, "边端消息不是合法 JSON");
        }
        if (root == null || !root.isObject()) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_JSON, "边端消息必须是 JSON 对象");
        }
        if (topic.startsWith("edge/config/pull/reply/") && root.path("chunked").asBoolean(false)) {
            AssembledConfig snapshot = assembleConfigSnapshot(topic, root);
            if (snapshot == null) return;
            try {
                ingestConfigSnapshot(topic, decodeConfigSnapshot(snapshot));
                completeConfigSnapshot(snapshot.token());
            } catch (EdgeMessageRejectedException exception) {
                discardConfigSnapshot(snapshot.token());
                throw exception;
            }
            return;
        }
        if (realtimeTopic(topic)) {
            ingestRealtime(topic, root);
        } else if (topic.startsWith("edge/config/pull/reply/")) {
            ingestConfigSnapshot(topic, root);
        } else if (topic.startsWith("edge/camera/status/")) {
            ingestCameraStatus(topic, root);
        } else if (topic.startsWith("edge/alarm/")) {
            ingestAlarm(topic, root);
        } else if (topic.startsWith("edge/system/diag/reply/")) {
            ingestDiagnosticReply(topic, root);
        } else if (topic.startsWith("edge/system/monitor/telemetry/")) {
            ingestSystemTelemetry(topic, root);
        } else if (topic.startsWith("edge/system/monitor/alert/")) {
            ingestSystemAlert(topic, root);
        } else if (topic.startsWith("edge/config/state/")) {
            ingestConfigState(topic, root);
        } else if (topic.startsWith("edge/status/")) {
            ingestStatus(topic, root);
        }
    }

    private void ingestRealtime(String topic, JsonNode root) {
        long startedAt = System.nanoTime();
        String topicMachineCode = topicTail(topic);
        String payloadMachineCode = safeId(root.path("machineCode").asText(""), "machineCode");
        String machineCode = payloadMachineCode.isBlank() ? topicMachineCode : payloadMachineCode;
        if (machineCode.isBlank() || (!payloadMachineCode.isBlank() && !payloadMachineCode.equals(topicMachineCode))) {
            throw rejected(EdgeMessageRejectedException.Reason.DEVICE_IDENTITY_MISMATCH,
                "边端消息 machineCode 与 topic 不一致");
        }
        long deviceId = registeredDeviceId(machineCode);
        Instant receivedAt = Instant.now();
        boolean fullSnapshot = topic.equals("edge/telemetry/full/" + machineCode);
        String sessionId = scopedRealtimeSession(topic, root);
        EdgeRuntimeRepository.RealtimeLease lease = sessionId == null ? null
            : repository.activeRealtimeLeaseForUpdate(sessionId, deviceId, receivedAt)
                .orElseThrow(() -> rejected(EdgeMessageRejectedException.Reason.INVALID_REALTIME_PAYLOAD,
                    "实时订阅会话未知、跨设备、已停止或已过期"));
        if (lease != null && (!root.path("machineCode").isTextual()
            || !machineCode.equals(root.path("machineCode").asText()))) {
            throw rejected(EdgeMessageRejectedException.Reason.DEVICE_IDENTITY_MISMATCH, "实时会话缺少明确设备身份");
        }
        int count = 0;
        List<EdgeRuntimeRepository.PointValue> points = new ArrayList<>();
        JsonNode meters = root.path("meters");
        if (!meters.isArray()) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_REALTIME_PAYLOAD,
                "实时快照缺少 meters");
        }
        for (JsonNode meter : meters) {
            String meterCode = safeId(meter.path("meterCode").asText(""), "meterCode");
            if (lease != null && (!meter.path("meterCode").isTextual()
                || !meterCode.equals(meter.path("meterCode").asText()))) {
                throw rejected(EdgeMessageRejectedException.Reason.INVALID_REALTIME_PAYLOAD, "实时会话表计身份非法");
            }
            if (lease != null && lease.meterCode() != null
                && !lease.meterCode().equals(meterCode)) {
                throw rejected(EdgeMessageRejectedException.Reason.INVALID_REALTIME_PAYLOAD,
                    "实时消息表计与订阅选择器不一致");
            }
            if (meterCode.isBlank() || !meter.path("values").isArray()) {
                if (lease != null) throw rejected(EdgeMessageRejectedException.Reason.INVALID_REALTIME_PAYLOAD,
                    "实时消息表计点位格式非法");
                continue;
            }
            for (JsonNode value : meter.path("values")) {
                if (++count > MAX_POINT_VALUES) {
                    throw rejected(EdgeMessageRejectedException.Reason.POINT_LIMIT,
                        "实时点位数量超过限制");
                }
                if (lease != null) validateScopedPoint(value);
                if (fullSnapshot) {
                    // History identity must survive MQTT redelivery; never substitute receipt time.
                    JsonNode ts = value.isArray() ? value.path(4) : value.path("ts");
                    if (!ts.isIntegralNumber() || !ts.canConvertToLong()) {
                        throw rejected(EdgeMessageRejectedException.Reason.INVALID_REALTIME_PAYLOAD,
                            "Full snapshot requires an integral source timestamp for every point");
                    }
                }
                DecodedRealtimePoint point = decodeRealtimePoint(value, receivedAt.toEpochMilli());
                if (point == null) continue;
                points.add(new EdgeRuntimeRepository.PointValue(deviceId, meterCode, point.index(),
                    point.pointCode(), point.name(), point.valueJson(), point.quality(),
                    point.sourceTs(), receivedAt, point.stale(), point.alarmActive(),
                    fullSnapshot && isNoSamplePlaceholder(value)));
            }
        }
        repository.upsertPoints(points, fullSnapshot);
        if (sessionId != null) {
            repository.upsertSessionPoints(sessionId, points);
            if (!points.isEmpty()) repository.markRealtimeLeaseActive(sessionId, deviceId);
        }
        if (count > 0) {
            repository.recordSeen(deviceId, receivedAt);
        }
        long elapsedMs = (System.nanoTime() - startedAt) / 1_000_000L;
        if (elapsedMs >= 1_000L) {
            log.warn("Slow realtime snapshot ingest; machineCode={}, sessionId={}, points={}, elapsedMs={}",
                machineCode, sessionId, points.size(), elapsedMs);
        } else {
            log.debug("Realtime snapshot ingested; machineCode={}, sessionId={}, points={}, elapsedMs={}",
                machineCode, sessionId, points.size(), elapsedMs);
        }
    }

    private void validateScopedPoint(JsonNode value) {
        JsonNode index = value.isArray() ? value.path(0) : value.path("index");
        JsonNode ts = value.isArray() ? value.path(4) : value.path("ts");
        JsonNode quality = value.isArray() ? value.path(3) : value.path("quality");
        JsonNode stale = value.isArray() ? value.path(6) : value.path("stale");
        JsonNode expires = value.isArray() ? value.path(5) : value.path("expireAt");
        JsonNode code = value.isArray() ? value.path(1) : value.path("pointCode");
        if ((!value.isObject() && !value.isArray()) || (value.isArray() && value.size() < 5)
            || !index.isIntegralNumber() || !index.canConvertToInt() || index.asInt() <= 0
            || !ts.isIntegralNumber() || !ts.canConvertToLong()
            || (!quality.isMissingNode() && !quality.isNull() && (!quality.isIntegralNumber() || !quality.canConvertToInt()))
            || (!stale.isMissingNode() && !stale.isBoolean())
            || (!expires.isMissingNode() && !expires.isNull() && (!expires.isIntegralNumber() || !expires.canConvertToLong()))
            || (!code.isMissingNode() && !code.isNull() && !code.isTextual())
            || (value.isObject() && !value.has("value"))) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_REALTIME_PAYLOAD,
                "实时会话点位格式非法");
        }
    }

    private String scopedRealtimeSession(String topic, JsonNode root) {
        String prefix = "edge/telemetry/realtime/";
        String topicDevice = topic.startsWith(prefix) ? topic.substring(prefix.length()) : "";
        if (topicDevice.isBlank() || topicDevice.contains("/") || !root.has("sessionId")) return null;
        JsonNode value = root.get("sessionId");
        if (value == null || !value.isTextual()) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_REALTIME_PAYLOAD,
                "实时消息 sessionId 格式非法");
        }
        String sessionId = value.asText();
        if (!REALTIME_SESSION.matcher(sessionId).matches()) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_REALTIME_PAYLOAD,
                "实时消息 sessionId 格式非法");
        }
        return sessionId;
    }

    private boolean isNoSamplePlaceholder(JsonNode point) {
        // Inspect the wire values before coercion: missing/null/zero are not interchangeable.
        if (!point.isObject() && !point.isArray()) return false;
        if (point.isArray() && point.size() != 7) return false;
        JsonNode index = point.isArray() ? point.path(0) : point.path("index");
        JsonNode code = point.isArray() ? point.path(1) : point.path("pointCode");
        JsonNode value = point.isArray() ? point.path(2) : point.path("value");
        JsonNode quality = point.isArray() ? point.path(3) : point.path("quality");
        JsonNode timestamp = point.isArray() ? point.path(4) : point.path("ts");
        JsonNode expires = point.isArray() ? point.path(5) : point.path("expireAt");
        JsonNode stale = point.isArray() ? point.path(6) : point.path("stale");
        return index.isIntegralNumber() && index.canConvertToInt() && index.intValue() > 0
            && code.isTextual() && !code.textValue().isBlank()
            && value.isNull() && zeroInteger(quality) && zeroInteger(timestamp)
            && zeroInteger(expires) && stale.isBoolean() && stale.booleanValue();
    }

    private boolean zeroInteger(JsonNode value) {
        return value.isIntegralNumber() && value.canConvertToLong() && value.longValue() == 0;
    }

    private DecodedRealtimePoint decodeRealtimePoint(JsonNode value, long receivedAtMs) {
        if (value.isArray()) {
            // Deployed edge compactArray wire format:
            // [index, pointCode, value, quality, ts, expireAt, stale]
            if (value.size() < 5) return null;
            int index = value.path(0).asInt(0);
            if (index <= 0) return null;
            JsonNode rawValue = value.path(2);
            Integer quality = value.path(3).isNumber() ? value.path(3).asInt() : null;
            return new DecodedRealtimePoint(index, limited(value.path(1).asText(null), 256), null,
                rawValue.isMissingNode() ? null : rawValue.toString(), quality,
                value.path(4).asLong(receivedAtMs), value.path(6).asBoolean(false), null);
        }
        if (!value.isObject()) return null;
        int index = value.path("index").asInt(0);
        if (index <= 0) return null;
        JsonNode alarm = value.path("alarm");
        Boolean alarmActive = nullableBoolean(value, "alarmActive");
        if (alarmActive == null) alarmActive = nullableBoolean(alarm, "active");
        return new DecodedRealtimePoint(index,
            limited(value.path("pointCode").asText(null), 256),
            limited(value.path("name").asText(null), 256),
            value.has("value") ? value.path("value").toString() : null,
            value.hasNonNull("quality") ? value.path("quality").asInt() : null,
            value.path("ts").asLong(receivedAtMs), value.path("stale").asBoolean(false),
            alarmActive);
    }

    private record DecodedRealtimePoint(int index, String pointCode, String name, String valueJson,
                                        Integer quality, long sourceTs, boolean stale,
                                        Boolean alarmActive) {
    }

    private boolean realtimeTopic(String topic) {
        return topic.startsWith("edge/telemetry/") || topic.startsWith("edge/system/monitor/points/");
    }

    private void ingestConfigSnapshot(String topic, JsonNode root) {
        String machineCode = requiredMatchingMachineCode(topic, root);
        long deviceId = registeredDeviceId(machineCode);
        String requestId = safeId(root.path("requestId").asText(""), "requestId");
        if (requestId.isBlank()) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "配置快照缺少 requestId");
        }
        long sourceTs = root.path("ts").asLong(System.currentTimeMillis());
        var summary = objectMapper.createObjectNode();
        summary.put("requestId", requestId);
        summary.put("machineCode", machineCode);
        if (root.hasNonNull("success")) summary.put("success", root.path("success").asBoolean());
        copyText(root, summary, "message", 500);
        copyLong(root, summary, "fileCount");
        copyLong(root, summary, "skippedFiles");
        copyLong(root, summary, "totalBytes");
        summary.put("ts", sourceTs);
        var files = summary.putArray("files");
        if (root.path("files").isArray()) {
            int count = 0;
            for (JsonNode sourceFile : root.path("files")) {
                if (++count > 256) {
                    throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "配置快照文件数量超过限制");
                }
                var file = files.addObject();
                copyText(sourceFile, file, "path", 512);
                copyLong(sourceFile, file, "sizeBytes");
                copyLong(sourceFile, file, "modifiedAtMs");
            }
        }
        Instant receivedAt = Instant.now();
        repository.upsertConfigSnapshot(deviceId, requestId, summary.toString(), sourceTs, receivedAt);
        repository.recordSeen(deviceId, receivedAt);
    }

    private AssembledConfig assembleConfigSnapshot(String topic, JsonNode root) {
        String machineCode = requiredMatchingMachineCode(topic, root);
        String requestId = safeId(root.path("requestId").asText(""), "requestId");
        if (requestId.isBlank()) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "配置快照分片元数据非法");
        }
        String key = machineCode + "::" + requestId;
        int chunkIndex = root.path("chunkIndex").asInt(0);
        int chunkCount = root.path("chunkCount").asInt(0);
        long totalBytes = root.path("totalBytes").asLong(0L);
        ConfigAssemblyToken beforeDecode;
        synchronized (configAssemblies) {
            ConfigAssembly current = configAssemblies.get(key);
            beforeDecode = current == null ? null : new ConfigAssemblyToken(key, current, current.version);
        }
        byte[] part;
        try {
            if (chunkCount <= 0 || chunkCount > MAX_CONFIG_CHUNKS || chunkIndex <= 0
                || chunkIndex > chunkCount || totalBytes <= 0 || totalBytes > MAX_CONFIG_BYTES) {
                throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "配置快照分片元数据非法");
            }
            part = configChunkBytes(root);
            if (part.length == 0 || part.length > maxPayloadBytes) {
                throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "配置快照分片内容非法");
            }
        } catch (EdgeMessageRejectedException exception) {
            if (beforeDecode != null) discardConfigSnapshot(beforeDecode);
            throw exception;
        }
        synchronized (configAssemblies) {
            long now = System.currentTimeMillis();
            configAssemblies.entrySet().removeIf(entry -> now - entry.getValue().updatedAt > CONFIG_CHUNK_TTL_MILLIS);
            try {
                ConfigAssembly assembly = configAssemblies.get(key);
                if (assembly == null) {
                    if (configAssemblies.size() >= 256) {
                        // Capacity pressure can clear; keep it retryable instead of consuming the message.
                        throw new IllegalArgumentException("等待组装的配置快照过多");
                    }
                    long reservedBytes = configAssemblies.values().stream()
                        .mapToLong(pending -> pending.totalBytes).sum();
                    if (reservedBytes + totalBytes > MAX_CONFIG_RESERVED_BYTES) {
                        throw new IllegalArgumentException("等待组装的配置快照预留大小超过限制");
                    }
                    assembly = new ConfigAssembly(machineCode, requestId, chunkCount, totalBytes, now);
                    configAssemblies.put(key, assembly);
                }
                if (assembly.chunkCount != chunkCount || assembly.totalBytes != totalBytes) {
                    throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "配置快照分片元数据发生变化");
                }
                byte[] previous = assembly.parts[chunkIndex - 1];
                if (previous != null && !Arrays.equals(previous, part)) {
                    throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "配置快照同索引分片内容发生变化");
                }
                if (previous == null) {
                    long receivedBytes = assembly.receivedBytes + part.length;
                    if (receivedBytes > totalBytes || receivedBytes > MAX_CONFIG_BYTES) {
                        throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "配置快照组装内容过大");
                    }
                    assembly.parts[chunkIndex - 1] = part;
                    assembly.receivedBytes = receivedBytes;
                    assembly.receivedParts++;
                }
                assembly.updatedAt = now;
                assembly.version++;
                if (assembly.receivedParts != chunkCount) return null;
                if (assembly.receivedBytes != totalBytes) {
                    throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "配置快照分片组装长度不一致");
                }
                return new AssembledConfig(new ConfigAssemblyToken(key, assembly, assembly.version),
                    assembly.parts.clone());
            } catch (EdgeMessageRejectedException exception) {
                configAssemblies.remove(key);
                throw exception;
            }
        }
    }

    private JsonNode decodeConfigSnapshot(AssembledConfig snapshot) {
        ConfigAssembly assembly = snapshot.token().assembly();
        ByteArrayOutputStream output = new ByteArrayOutputStream((int) assembly.totalBytes);
        for (byte[] bytes : snapshot.parts()) output.write(bytes, 0, bytes.length);
        try {
            JsonNode assembled = objectMapper.readTree(output.toByteArray());
            if (assembled == null || !assembled.isObject()) {
                throw rejected(EdgeMessageRejectedException.Reason.INVALID_JSON, "配置快照分片组装结果必须是 JSON 对象");
            }
            if (!assembly.machineCode.equals(assembled.path("machineCode").asText())
                || !assembly.requestId.equals(assembled.path("requestId").asText())) {
                throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "配置快照分片身份不一致");
            }
            return assembled;
        } catch (JsonProcessingException exception) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_JSON, "配置快照分片组装结果不是合法 JSON");
        } catch (java.io.IOException exception) {
            throw new IllegalStateException("配置快照分片组装结果无法读取", exception);
        }
    }

    private void completeConfigSnapshot(ConfigAssemblyToken snapshot) {
        if (!TransactionSynchronizationManager.isActualTransactionActive()) {
            discardConfigSnapshot(snapshot);
        } else if (TransactionSynchronizationManager.isSynchronizationActive()) {
            TransactionSynchronizationManager.registerSynchronization(new TransactionSynchronization() {
                @Override
                public void afterCompletion(int status) {
                    if (status == STATUS_COMMITTED) discardConfigSnapshot(snapshot);
                }
            });
        }
    }

    private void discardConfigSnapshot(ConfigAssemblyToken snapshot) {
        synchronized (configAssemblies) {
            // Even identical retransmissions supersede cleanup owned by an older in-flight transaction.
            if (configAssemblies.get(snapshot.key()) == snapshot.assembly()
                && snapshot.assembly().version == snapshot.version()) {
                configAssemblies.remove(snapshot.key());
            }
        }
    }

    private byte[] configChunkBytes(JsonNode root) {
        String hex = root.path("payloadHex").asText("");
        if (!hex.isBlank()) {
            if ((hex.length() & 1) != 0 || hex.length() > maxPayloadBytes * 2L
                || !hex.matches("[0-9A-Fa-f]+")) {
                throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "配置快照 payloadHex 非法");
            }
            return java.util.HexFormat.of().parseHex(hex);
        }
        return root.path("payloadPart").asText("").getBytes(StandardCharsets.UTF_8);
    }

    private void ingestCameraStatus(String topic, JsonNode root) {
        String machineCode = requiredMatchingMachineCode(topic, root);
        long deviceId = registeredDeviceId(machineCode);
        String cameraCode = safeId(root.path("cameraCode").asText(""), "cameraCode");
        if (cameraCode.isBlank()) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "摄像头状态缺少 cameraCode");
        }
        long sourceTs = root.path("ts").asLong(System.currentTimeMillis());
        var sanitized = objectMapper.createObjectNode();
        sanitized.put("machineCode", machineCode);
        sanitized.put("cameraCode", cameraCode);
        if (root.hasNonNull("online")) sanitized.put("online", root.path("online").asBoolean());
        if (root.hasNonNull("fps")) sanitized.put("fps", root.path("fps").asDouble());
        if (root.hasNonNull("bitrateKbps")) sanitized.put("bitrateKbps", root.path("bitrateKbps").asDouble());
        if (root.hasNonNull("errorCode")) sanitized.put("errorCode", root.path("errorCode").asInt());
        copyText(root, sanitized, "name", 256);
        copyRedactedUrl(root, sanitized, "streamUrl");
        sanitized.put("ts", sourceTs);
        Instant receivedAt = Instant.now();
        repository.upsertCameraStatus(deviceId, cameraCode, sanitized.toString(), sourceTs, receivedAt);
        repository.recordSeen(deviceId, receivedAt);
    }

    private void ingestAlarm(String topic, JsonNode root) {
        String machineCode = requiredMatchingMachineCode(topic, root);
        long deviceId = registeredDeviceId(machineCode);
        String meterCode = safeId(root.path("meterCode").asText(""), "meterCode");
        String alarmType = safeId(root.path("alarmType").asText(""), "alarmType");
        int pointIndex = root.path("index").asInt(0);
        if (meterCode.isBlank() || alarmType.isBlank() || pointIndex <= 0 || !root.hasNonNull("active")) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "边端告警缺少必要字段");
        }
        long sourceTs = root.path("ts").asLong(System.currentTimeMillis());
        alarms.ingestFromTrustedEdge(new AlarmApplicationService.IngestCommand(machineCode, null, meterCode,
            pointIndex, limited(root.path("pointCode").asText(null), 256), alarmType,
            root.path("active").asBoolean(), root.hasNonNull("value") ? root.path("value").asDouble() : null,
            root.hasNonNull("quality") ? root.path("quality").asInt() : null,
            Instant.ofEpochMilli(sourceTs), null, null, null));
        repository.recordSeen(deviceId, Instant.now());
    }

    private void ingestDiagnosticReply(String topic, JsonNode root) {
        String machineCode = requiredMatchingMachineCode(topic, root);
        long deviceId = registeredDeviceId(machineCode);
        if ("admission_rejection".equals(root.path("replyKind").asText()) ||
            "cmdId_conflict".equals(root.path("reason").asText())) return;
        if (root.has("replyKind") && !"terminal".equals(root.path("replyKind").asText()) ||
            !root.path("success").isBoolean() || !integerDiagnosticField(root, "exitCode", true) ||
            !integerDiagnosticField(root, "signal", false) ||
            !integerDiagnosticField(root, "requestTs", true) ||
            !optionalDiagnosticType(root, "reason", JsonNodeType.STRING) ||
            !optionalDiagnosticType(root, "timedOut", JsonNodeType.BOOLEAN) ||
            !optionalDiagnosticType(root, "cleanupConfirmed", JsonNodeType.BOOLEAN) ||
            !optionalDiagnosticType(root, "truncated", JsonNodeType.BOOLEAN) ||
            !optionalDiagnosticType(root, "stdout", JsonNodeType.STRING) ||
            !optionalDiagnosticType(root, "stderr", JsonNodeType.STRING) ||
            !boundedDiagnosticText(root, "reason", 64) ||
            root.has("reason") && !DIAGNOSTIC_REASONS.contains(root.path("reason").asText()) ||
            !boundedDiagnosticText(root, "command", 128) ||
            !boundedDiagnosticText(root, "arg", 512) ||
            !boundedDiagnosticText(root, "operator", 120)) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "诊断终态字段类型非法");
        }
        int exitCode = root.path("exitCode").asInt();
        int signal = root.path("signal").asInt(0);
        String reason = root.path("reason").asText("");
        boolean success = root.path("success").asBoolean();
        if (signal < 0 || success &&
            (exitCode != 0 || signal != 0 || root.path("timedOut").asBoolean(false) ||
                root.has("cleanupConfirmed") && !root.path("cleanupConfirmed").asBoolean() ||
                !reason.isEmpty() && !"exit".equals(reason)) ||
            !success && exitCode == 0 ||
            "timeout".equals(reason) && !root.path("timedOut").asBoolean(false) ||
            "signal".equals(reason) && signal == 0 ||
            "cleanup_unknown".equals(reason) &&
                (!root.has("cleanupConfirmed") || root.path("cleanupConfirmed").asBoolean())) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "诊断终态字段矛盾");
        }
        String commandId = safeId(root.path("cmdId").asText(""), "cmdId");
        if (commandId.isBlank()) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "诊断回执缺少 cmdId");
        }
        var binding = repository.diagnosticRequest(commandId).orElseThrow(() ->
            rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "诊断请求身份未登记"));
        if (binding.deviceId() != deviceId ||
            !binding.command().equals(root.path("command").asText(null)) ||
            !binding.argument().equals(root.path("arg").asText(null)) ||
            !binding.operator().equals(root.path("operator").asText(null)) ||
            binding.requestTs() != root.path("requestTs").asLong()) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "诊断回执与已登记请求不匹配");
        }
        var unsafeResult = objectMapper.createObjectNode();
        unsafeResult.put("exitCode", exitCode);
        for (String field : new String[] {"stdout", "stderr"}) {
            if (root.path(field).isTextual()) unsafeResult.put(field, root.path(field).asText());
        }
        for (String field : new String[] {"reason", "command", "arg", "operator", "replyKind"}) {
            if (root.path(field).isTextual()) unsafeResult.put(field, root.path(field).asText());
        }
        if (root.has("signal")) unsafeResult.put("signal", signal);
        for (String field : new String[] {"timedOut", "cleanupConfirmed", "truncated"}) {
            if (root.has(field)) unsafeResult.put(field, root.path(field).asBoolean());
        }
        copyLong(root, unsafeResult, "requestTs");
        copyLong(root, unsafeResult, "ts");
        DiagnosticResultSanitizer.SanitizedDiagnostic cleaned = diagnosticSanitizer.sanitize(unsafeResult);
        JsonNode cleanResult;
        try {
            cleanResult = objectMapper.readTree(cleaned.resultJson());
        } catch (java.io.IOException exception) {
            throw new IllegalStateException("脱敏后的诊断结果无法解析", exception);
        }
        var sanitized = objectMapper.createObjectNode();
        sanitized.put("cmdId", commandId);
        sanitized.put("machineCode", machineCode);
        sanitized.put("success", root.path("success").asBoolean());
        if (cleanResult.hasNonNull("exitCode")) sanitized.put("exitCode", cleanResult.path("exitCode").asInt());
        for (String field : new String[] {"reason", "command", "arg", "operator", "replyKind"}) {
            if (cleanResult.hasNonNull(field)) sanitized.put(field, cleanResult.path(field).asText());
        }
        if (cleanResult.hasNonNull("signal")) sanitized.put("signal", cleanResult.path("signal").asInt());
        for (String field : new String[] {"timedOut", "cleanupConfirmed"}) {
            if (cleanResult.hasNonNull(field)) sanitized.put(field, cleanResult.path(field).asBoolean());
        }
        sanitized.put("stdout", cleanResult.path("stdout").asText(""));
        sanitized.put("stderr", cleanResult.path("stderr").asText(""));
        if (cleanResult.hasNonNull("requestTs")) sanitized.put("requestTs", cleanResult.path("requestTs").asLong());
        if (cleanResult.hasNonNull("replyTs")) sanitized.put("replyTs", cleanResult.path("replyTs").asLong());
        sanitized.put("redacted", cleaned.redacted());
        sanitized.put("truncated", cleaned.truncated());
        Instant receivedAt = Instant.now();
        repository.insertDiagnosticResult(deviceId, commandId, sanitized.toString(), receivedAt);
        repository.recordSeen(deviceId, receivedAt);
    }

    private boolean optionalDiagnosticType(JsonNode root, String field, JsonNodeType type) {
        JsonNode value = root.get(field);
        return value == null || value.getNodeType() == type;
    }

    private boolean integerDiagnosticField(JsonNode root, String field, boolean required) {
        JsonNode value = root.get(field);
        return value == null ? !required : value.isIntegralNumber() &&
            ("requestTs".equals(field) ? value.canConvertToLong() : value.canConvertToInt());
    }

    private boolean boundedDiagnosticText(JsonNode root, String field, int max) {
        JsonNode value = root.get(field);
        return value == null || value.isTextual() && value.asText().length() <= max;
    }

    private void ingestSystemTelemetry(String topic, JsonNode root) {
        String machineCode = requiredMatchingMachineCode(topic, root);
        long deviceId = registeredDeviceId(machineCode);
        long sourceTs = root.path("ts").asLong(System.currentTimeMillis());
        var sanitized = objectMapper.createObjectNode();
        sanitized.put("machineCode", machineCode);
        sanitized.put("type", "system-monitor");
        copyDouble(root, sanitized, "cpuUsage");
        copyDouble(root, sanitized, "memUsage");
        copyDouble(root, sanitized, "diskUsage");
        copyDouble(root, sanitized, "load1");
        copyLong(root, sanitized, "processCount");
        JsonNode sourceCellular = root.path("cellular");
        if (sourceCellular.isObject()) {
            var cellular = sanitized.putObject("cellular");
            copyBoolean(sourceCellular, cellular, "enabled");
            copyBoolean(sourceCellular, cellular, "present");
            copyBoolean(sourceCellular, cellular, "registered");
            copyBoolean(sourceCellular, cellular, "connected");
            copyBoolean(sourceCellular, cellular, "simReady");
            copyText(sourceCellular, cellular, "operator", 128);
            copyText(sourceCellular, cellular, "accessTech", 64);
            copyText(sourceCellular, cellular, "interfaceName", 64);
            copyDouble(sourceCellular, cellular, "signalPercent");
            copyDouble(sourceCellular, cellular, "rssiDbm");
            copyLong(sourceCellular, cellular, "ts");
            JsonNode sourceRoute = sourceCellular.path("route");
            if (sourceRoute.isObject()) {
                var route = cellular.putObject("route");
                copyBoolean(sourceRoute, route, "available");
                copyBoolean(sourceRoute, route, "failoverEnabled");
                copyBoolean(sourceRoute, route, "preferCellular");
                copyBoolean(sourceRoute, route, "usingCellular");
                copyText(sourceRoute, route, "mode", 32);
                copyText(sourceRoute, route, "activeInterface", 64);
                copyText(sourceRoute, route, "selectedWiredInterface", 64);
                copyText(sourceRoute, route, "lastResult", 32);
                copyText(sourceRoute, route, "lastChecked", 64);
            }
        }
        var services = sanitized.putArray("services");
        if (root.path("services").isArray()) {
            int count = 0;
            for (JsonNode sourceService : root.path("services")) {
                if (++count > 128) break;
                var service = services.addObject();
                copyText(sourceService, service, "name", 128);
                copyBoolean(sourceService, service, "running");
            }
        }
        sanitized.put("ts", sourceTs);
        Instant receivedAt = Instant.now();
        repository.upsertSystemTelemetry(deviceId, sanitized.toString(), sourceTs, receivedAt);
        repository.recordSeen(deviceId, receivedAt);
    }

    private void ingestSystemAlert(String topic, JsonNode root) {
        String machineCode = requiredMatchingMachineCode(topic, root);
        long deviceId = registeredDeviceId(machineCode);
        String metric = safeId(root.path("metric").asText(""), "metric");
        if (metric.isBlank() || !root.hasNonNull("active")) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "系统资源告警缺少必要字段");
        }
        long sourceTs = root.path("ts").asLong(System.currentTimeMillis());
        String level = root.path("level").asText("").trim().toUpperCase(java.util.Locale.ROOT);
        String severity = switch (level) {
            case "CRITICAL", "ERROR", "FATAL" -> "CRITICAL";
            case "WARNING", "WARN" -> "WARNING";
            case "INFO" -> "INFO";
            default -> null;
        };
        int pointIndex = Math.floorMod(metric.hashCode(), 1_000_000) + 1;
        alarms.ingestFromTrustedEdge(new AlarmApplicationService.IngestCommand(machineCode, null, "SYSTEM",
            pointIndex, metric, metric, root.path("active").asBoolean(),
            root.hasNonNull("value") ? root.path("value").asDouble() : null, null,
            Instant.ofEpochMilli(sourceTs), severity, null,
            limited(root.path("message").asText(null), 1000)));
        repository.recordSeen(deviceId, Instant.now());
    }

    private void ingestStatus(String topic, JsonNode root) {
        String machineCode = topicTail(topic);
        String payloadMachineCode = safeId(root.path("machineCode").asText(""), "machineCode");
        if (machineCode.isBlank() || (!payloadMachineCode.isBlank() && !machineCode.equals(payloadMachineCode))) {
            throw rejected(EdgeMessageRejectedException.Reason.DEVICE_IDENTITY_MISMATCH,
                "边端状态消息 machineCode 与 topic 不一致");
        }
        long deviceId = registeredDeviceId(machineCode);
        String event = root.path("event").asText("");
        if (event.equals("realtime-session-started") || event.equals("realtime-session-rejected")) {
            String sessionId = root.path("sessionId").asText("");
            if (!topic.equals("edge/status/" + machineCode)
                || !root.path("machineCode").isTextual() || !machineCode.equals(root.path("machineCode").asText())
                || !root.path("service").asText("").equals("mqtt-driver")
                || !root.path("sessionId").isTextual() || !REALTIME_SESSION.matcher(sessionId).matches()
                || !root.path("ts").isNumber()) {
                throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "实时反馈身份或格式非法");
            }
            if (event.equals("realtime-session-rejected")) {
                String reason = root.path("reasonCode").asText("");
                if (!reason.equals("CAPACITY_REACHED") && !reason.equals("INVALID_REQUEST")) {
                    throw rejected(EdgeMessageRejectedException.Reason.INVALID_PAYLOAD, "实时反馈拒绝原因非法");
                }
                repository.rejectRealtimeLease(sessionId, deviceId, reason);
            } else {
                repository.markRealtimeLeaseActive(sessionId, deviceId);
            }
        }
        repository.recordSeen(deviceId, Instant.now());
    }

    private void ingestConfigState(String topic, JsonNode root) {
        String machineCode = requiredMatchingMachineCode(topic, root);
        long deviceId = registeredDeviceId(machineCode);
        String version = firstText(root, "actualVersion", "appliedVersion", "version");
        String sha256 = firstText(root, "actualSha256", "appliedSha256", "sha256");
        String runtimeStatus = root.path("runtimeStatus").asText("UNKNOWN");
        String taskId = firstText(root, "taskId", "jobId");
        long sourceTs = root.path("ts").asLong(System.currentTimeMillis());
        deploymentStates.reportActual(deviceId, version, sha256, runtimeStatus, sourceTs, taskId);
        repository.recordSeen(deviceId, Instant.now());
    }

    private String firstText(JsonNode root, String... fields) {
        for (String field : fields) {
            String value = root.path(field).asText("").trim();
            if (!value.isBlank()) return value;
        }
        return null;
    }

    private String requiredMatchingMachineCode(String topic, JsonNode root) {
        String fromTopic = topicTail(topic);
        String fromPayload = safeId(root.path("machineCode").asText(""), "machineCode");
        if (fromTopic.isBlank() || fromPayload.isBlank() || !fromTopic.equals(fromPayload)) {
            throw rejected(EdgeMessageRejectedException.Reason.DEVICE_IDENTITY_MISMATCH,
                "边端消息 machineCode 与 topic 不一致");
        }
        return fromPayload;
    }

    private long registeredDeviceId(String machineCode) {
        return repository.findDeviceId(machineCode)
            .orElseThrow(() -> rejected(EdgeMessageRejectedException.Reason.UNREGISTERED_DEVICE,
                "未登记的边端设备: " + machineCode));
    }

    private static EdgeMessageRejectedException rejected(EdgeMessageRejectedException.Reason reason,
                                                         String message) {
        return new EdgeMessageRejectedException(reason, message);
    }

    private void copyText(JsonNode source, com.fasterxml.jackson.databind.node.ObjectNode target,
                          String field, int max) {
        String value = limited(source.path(field).asText(null), max);
        if (value != null) target.put(field, value);
    }

    private void copyLong(JsonNode source, com.fasterxml.jackson.databind.node.ObjectNode target,
                          String field) {
        if (source.hasNonNull(field)) target.put(field, source.path(field).asLong());
    }

    private void copyDouble(JsonNode source, com.fasterxml.jackson.databind.node.ObjectNode target,
                            String field) {
        if (source.hasNonNull(field) && source.path(field).isNumber()) {
            target.put(field, source.path(field).asDouble());
        }
    }

    private void copyBoolean(JsonNode source, com.fasterxml.jackson.databind.node.ObjectNode target,
                             String field) {
        if (source.hasNonNull(field)) target.put(field, source.path(field).asBoolean());
    }

    private void copyRedactedUrl(JsonNode source, com.fasterxml.jackson.databind.node.ObjectNode target,
                                 String field) {
        String value = limited(source.path(field).asText(null), 2048);
        if (value != null) target.put(field, redactUrl(value));
    }

    private String redactUrl(String value) {
        return value.replaceFirst("(?i)(://)[^/@\\s]+@", "$1***:***@");
    }

    private String topicTail(String topic) {
        int separator = topic.lastIndexOf('/');
        return separator < 0 ? "" : safeId(topic.substring(separator + 1), "topic machineCode");
    }

    private String safeId(String value, String field) {
        String normalized = value == null ? "" : value.trim();
        if (!normalized.isBlank() && (normalized.length() > 128
            || !normalized.matches("[A-Za-z0-9._:-]+"))) {
            throw rejected(EdgeMessageRejectedException.Reason.INVALID_IDENTIFIER,
                field + " 格式非法");
        }
        return normalized;
    }

    private String limited(String value, int max) {
        if (value == null || value.isBlank()) return null;
        String normalized = value.trim();
        return normalized.length() <= max ? normalized : normalized.substring(0, max);
    }

    private Boolean nullableBoolean(JsonNode node, String field) {
        return node != null && node.hasNonNull(field) ? node.path(field).asBoolean() : null;
    }

    private static final class ConfigAssembly {
        private final String machineCode;
        private final String requestId;
        private final int chunkCount;
        private final long totalBytes;
        private final byte[][] parts;
        private long updatedAt;
        private long version;
        private long receivedBytes;
        private int receivedParts;

        private ConfigAssembly(String machineCode, String requestId, int chunkCount,
                               long totalBytes, long updatedAt) {
            this.machineCode = machineCode;
            this.requestId = requestId;
            this.chunkCount = chunkCount;
            this.totalBytes = totalBytes;
            this.parts = new byte[chunkCount][];
            this.updatedAt = updatedAt;
        }
    }

    private record ConfigAssemblyToken(String key, ConfigAssembly assembly, long version) {
    }

    private record AssembledConfig(ConfigAssemblyToken token, byte[][] parts) {
    }
}
