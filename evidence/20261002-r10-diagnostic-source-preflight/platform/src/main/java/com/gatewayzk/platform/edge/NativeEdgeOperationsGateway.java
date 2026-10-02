package com.gatewayzk.platform.edge;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import org.springframework.stereotype.Service;

import java.time.Instant;
import java.nio.charset.StandardCharsets;
import java.util.Optional;
import java.util.UUID;

@Service
public class NativeEdgeOperationsGateway implements EdgeOperationsGateway {

    private static final int CONTROL_QOS = 2;
    private static final String DIAG_REQUEST_TOPIC = "edge/system/diag/request";
    private static final String CONFIG_PULL_REQUEST_TOPIC = "edge/config/pull/request";
    private static final String SYSTEM_MONITOR_REQUEST_TOPIC = "edge/system/monitor/request";
    private static final String REALTIME_REQUEST_TOPIC = "edge/telemetry/realtime/request";

    private final EdgeCommandTransport transport;
    private final EdgeRuntimeRepository repository;
    private final ObjectMapper objectMapper;

    public NativeEdgeOperationsGateway(EdgeCommandTransport transport,
                                       EdgeRuntimeRepository repository,
                                       ObjectMapper objectMapper) {
        this.transport = transport;
        this.repository = repository;
        this.objectMapper = objectMapper;
    }

    @Override
    public boolean ready() {
        return transport.ready();
    }

    @Override
    public String executeDiagnostic(String machineCode, String operatorName, String commandName,
                                    String service, int tailLines) {
        requireReady();
        String commandId = "DIAG_" + UUID.randomUUID();
        String operator = normalizeDiagnosticOperator(operatorName);
        int boundedTail = Math.max(1, Math.min(500, tailLines));
        long requestTs = Instant.now().toEpochMilli();
        var payload = objectMapper.createObjectNode();
        payload.put("cmdId", commandId);
        payload.put("machineCode", machineCode);
        payload.put("operator", operator);
        payload.put("command", commandName);
        if (service != null && !service.isBlank()) payload.put("service", service);
        payload.put("tailLines", boundedTail);
        payload.put("requestTs", requestTs);
        long deviceId = repository.findDeviceId(machineCode)
            .orElseThrow(() -> new IllegalArgumentException("诊断目标设备未登记"));
        String argument = "systemctl_status".equals(commandName) ? service :
            "journal_tail".equals(commandName) ? Integer.toString(boundedTail) : "";
        repository.reserveDiagnosticRequest(commandId, deviceId, commandName,
            argument == null ? "" : argument, operator, requestTs);
        publish(DIAG_REQUEST_TOPIC + "/" + machineCode, payload);
        return commandId;
    }

    @Override
    public Optional<JsonNode> findDiagnosticResult(String machineCode, String commandId) {
        return repository.diagnosticResult(machineCode, commandId);
    }

    @Override
    public String requestConfigSnapshot(String machineCode) {
        requireReady();
        String requestId = "CONFIG_" + UUID.randomUUID();
        var payload = objectMapper.createObjectNode();
        payload.put("requestId", requestId);
        payload.put("machineCode", machineCode);
        payload.put("ts", Instant.now().toEpochMilli());
        publish(CONFIG_PULL_REQUEST_TOPIC + "/" + machineCode, payload);
        return requestId;
    }

    @Override
    public SystemTelemetryRequest requestSystemTelemetry(String machineCode, String requestId, int intervalMs, int ttlSec) {
        requireReady();
        long requestedAt = Instant.now().toEpochMilli();
        var payload = objectMapper.createObjectNode();
        payload.put("action", "subscribe");
        payload.put("machineCode", machineCode);
        payload.put("sessionId", requestId);
        payload.put("intervalMs", intervalMs);
        payload.put("ttlSec", ttlSec);
        payload.put("ts", requestedAt);
        publish(SYSTEM_MONITOR_REQUEST_TOPIC + "/" + machineCode, payload);
        return new SystemTelemetryRequest(requestId, requestedAt, ttlSec);
    }

    @Override
    public void unsubscribeSystemTelemetry(String machineCode, String sessionId) {
        requireReady();
        var payload = objectMapper.createObjectNode();
        payload.put("action", "unsubscribe");
        payload.put("machineCode", machineCode);
        payload.put("sessionId", sessionId);
        payload.put("ts", Instant.now().toEpochMilli());
        publish(SYSTEM_MONITOR_REQUEST_TOPIC + "/" + machineCode, payload);
    }

    @Override
    public void subscribeRealtime(String machineCode, String sessionId, String meterCode,
                                  int intervalMs, int ttlSec) {
        requireReady();
        var payload = objectMapper.createObjectNode();
        payload.put("action", "subscribe");
        payload.put("machineCode", machineCode);
        payload.put("sessionId", sessionId);
        if (meterCode != null && !meterCode.isBlank()) payload.put("meterCode", meterCode);
        payload.put("intervalMs", intervalMs);
        payload.put("ttlSec", ttlSec);
        payload.put("ts", Instant.now().toEpochMilli());
        publish(REALTIME_REQUEST_TOPIC + "/" + machineCode, payload);
    }

    @Override
    public void unsubscribeRealtime(String machineCode, String sessionId) {
        requireReady();
        var payload = objectMapper.createObjectNode();
        payload.put("action", "unsubscribe");
        payload.put("machineCode", machineCode);
        payload.put("sessionId", sessionId);
        payload.put("ts", Instant.now().toEpochMilli());
        publish(REALTIME_REQUEST_TOPIC + "/" + machineCode, payload);
    }

    private void publish(String topic, JsonNode payload) {
        try {
            transport.publish(topic, objectMapper.writeValueAsBytes(payload), CONTROL_QOS);
        } catch (JsonProcessingException exception) {
            throw new EdgeTransportException("边端控制消息序列化失败", exception, false);
        }
    }

    private void requireReady() {
        if (!ready()) {
            throw new EdgeTransportException("边端 MQTT 原生通道未连接", null, false);
        }
    }

    public static String normalizeDiagnosticOperator(String value) {
        String normalized = value == null ? "platform-operator" : value.trim();
        if (normalized.isBlank()) return "platform-operator";
        int end = 0;
        int bytes = 0;
        while (end < normalized.length()) {
            int codePoint = normalized.codePointAt(end);
            int length = new String(Character.toChars(codePoint)).getBytes(StandardCharsets.UTF_8).length;
            if (bytes + length > 120) break;
            bytes += length;
            end += Character.charCount(codePoint);
        }
        return normalized.substring(0, end);
    }
}
