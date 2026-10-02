package com.gatewayzk.platform.edge;

import org.springframework.boot.context.properties.ConfigurationProperties;

@ConfigurationProperties("platform.edge-mqtt")
public record EdgeMqttProperties(
    boolean enabled,
    String broker,
    String clientId,
    String username,
    String password,
    boolean cleanSession,
    int connectionTimeoutSeconds,
    int keepAliveSeconds,
    int qos,
    int controlQos,
    int maxPayloadBytes,
    Tls tls,
    String machineCode
) {
    public static final int MAX_DIAGNOSTIC_REPLY_BYTES = 6 * 1024 * 1024 + 4096;

    public int inboundPayloadLimit(String topic) {
        return topic != null && topic.startsWith("edge/system/diag/reply/")
            ? MAX_DIAGNOSTIC_REPLY_BYTES : maxPayloadBytes;
    }

    public EdgeMqttProperties {
        broker = defaultValue(broker, "tcp://127.0.0.1:1883");
        clientId = defaultValue(clientId, "gateway-platform-next-edge");
        username = username == null ? "" : username;
        password = password == null ? "" : password;
        connectionTimeoutSeconds = Math.max(1, connectionTimeoutSeconds);
        keepAliveSeconds = Math.max(5, keepAliveSeconds);
        qos = clampQos(qos);
        controlQos = clampQos(controlQos);
        maxPayloadBytes = maxPayloadBytes <= 0 ? 4 * 1024 * 1024 : Math.min(maxPayloadBytes, 4 * 1024 * 1024);
        tls = tls == null ? new Tls("", "", "") : tls;
        machineCode = machineCode == null ? "" : machineCode;
        if (!machineCode.isEmpty() && !machineCode.matches("[A-Za-z0-9._:-]{1,128}")) {
            throw new IllegalArgumentException("MQTT machine-code must be one exact device identifier");
        }
        // A persistent client may retain broad subscriptions from an earlier configuration.
        if (!machineCode.isEmpty() && !cleanSession) {
            throw new IllegalArgumentException("Scoped MQTT requires clean-session=true");
        }
    }

    private static int clampQos(int value) {
        return Math.max(0, Math.min(2, value));
    }

    private static String defaultValue(String value, String fallback) {
        return value == null || value.isBlank() ? fallback : value.trim();
    }

    public record Tls(String caFile, String clientCertificateFile, String clientPrivateKeyFile) {
        public Tls {
            caFile = normalize(caFile);
            clientCertificateFile = normalize(clientCertificateFile);
            clientPrivateKeyFile = normalize(clientPrivateKeyFile);
            if (clientCertificateFile.isBlank() != clientPrivateKeyFile.isBlank()) {
                throw new IllegalArgumentException("边端 MQTT TLS 客户端证书和私钥必须同时配置");
            }
        }

        public boolean customSocketFactoryRequired() {
            return !caFile.isBlank() || !clientCertificateFile.isBlank();
        }

        private static String normalize(String value) {
            return value == null ? "" : value.trim();
        }
    }
}
