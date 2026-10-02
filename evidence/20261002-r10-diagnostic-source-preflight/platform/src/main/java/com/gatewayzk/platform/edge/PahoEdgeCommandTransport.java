package com.gatewayzk.platform.edge;

import com.gatewayzk.platform.configrev.PemTlsSocketFactory;
import jakarta.annotation.PostConstruct;
import jakarta.annotation.PreDestroy;
import org.eclipse.paho.client.mqttv3.IMqttDeliveryToken;
import org.eclipse.paho.client.mqttv3.IMqttToken;
import org.eclipse.paho.client.mqttv3.MqttAsyncClient;
import org.eclipse.paho.client.mqttv3.MqttCallbackExtended;
import org.eclipse.paho.client.mqttv3.MqttConnectOptions;
import org.eclipse.paho.client.mqttv3.MqttException;
import org.eclipse.paho.client.mqttv3.MqttMessage;
import org.eclipse.paho.client.mqttv3.persist.MemoryPersistence;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Component;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

@Component
public class PahoEdgeCommandTransport implements EdgeCommandTransport {

    private static final Logger log = LoggerFactory.getLogger(PahoEdgeCommandTransport.class);
    private static final List<String> DATA_TOPICS = List.of(
        "edge/telemetry/+",
        "edge/telemetry/realtime/+",
        "edge/telemetry/full/+",
        "edge/system/monitor/points/+",
        "edge/system/monitor/alert/+",
        "edge/alarm/+",
        "edge/config/state/+",
        "edge/camera/status/+",
        "edge/status/+"
    );
    private static final List<String> CONTROL_TOPICS = List.of(
        "edge/system/monitor/reply/+",
        "edge/system/monitor/telemetry/+",
        "edge/system/diag/reply/+",
        "edge/config/pull/reply/+"
    );

    private final EdgeMqttProperties properties;
    private final EdgeMessageIngestService ingress;
    private final MqttClientFactory clientFactory;
    private final Object connectionLock = new Object();
    private final ConcurrentHashMap<EdgeMessageRejectedException.Reason, AtomicLong> rejectionCounts =
        new ConcurrentHashMap<>();
    private final AtomicLong processingFailureCount = new AtomicLong();
    private volatile MqttAsyncClient controlClient;
    private volatile MqttAsyncClient dataClient;
    private volatile boolean controlSubscribed;
    private volatile boolean dataSubscribed;

    @Autowired
    public PahoEdgeCommandTransport(EdgeMqttProperties properties, EdgeMessageIngestService ingress) {
        this(properties, ingress, (broker, clientId) ->
            new MqttAsyncClient(broker, clientId, new MemoryPersistence()));
    }

    PahoEdgeCommandTransport(EdgeMqttProperties properties, EdgeMessageIngestService ingress,
                             MqttClientFactory clientFactory) {
        this.properties = properties;
        this.ingress = ingress;
        this.clientFactory = clientFactory;
    }

    @PostConstruct
    public void start() {
        if (properties.enabled()) tryConnect();
    }

    @Scheduled(fixedDelayString = "${platform.edge-mqtt.reconnect-delay-ms:10000}")
    public void maintainConnection() {
        if (properties.enabled() && (!controlReady() || !dataReady())) tryConnect();
    }

    @Override
    public boolean ready() {
        return properties.enabled() && controlReady();
    }

    @Override
    public void publish(String topic, byte[] payload, int qos) {
        validate(topic, payload, qos);
        ensureControlConnected();
        MqttAsyncClient current = controlClient;
        MqttMessage message = new MqttMessage(payload == null ? new byte[0] : payload);
        message.setQos(qos);
        message.setRetained(false);
        IMqttDeliveryToken delivery;
        try {
            delivery = current.publish(topic, message);
        } catch (MqttException exception) {
            invalidate(Channel.CONTROL, current);
            throw new EdgeTransportException("边端 MQTT 消息发布失败", exception, false);
        }
        try {
            delivery.waitForCompletion(Duration.ofSeconds(15).toMillis());
        } catch (MqttException exception) {
            invalidate(Channel.CONTROL, current);
            throw new EdgeTransportException("边端 MQTT 消息交付结果不确定", exception, true);
        }
    }

    private void tryConnect() {
        try {
            ensureConnected();
        } catch (RuntimeException exception) {
            log.warn("Native edge MQTT is not connected; broker={}, failure={}",
                properties.broker(), failureCategory(exception));
        }
    }

    private void ensureConnected() {
        if (!properties.enabled()) {
            throw new EdgeTransportException("边端 MQTT 原生通道未启用", null, false);
        }
        synchronized (connectionLock) {
            try {
                ensureChannelConnected(Channel.CONTROL);
                ensureChannelConnected(Channel.DATA);
            } catch (MqttException exception) {
                throw new EdgeTransportException("无法连接边端 MQTT 原生通道", exception, false);
            }
        }
    }

    private void ensureControlConnected() {
        if (!properties.enabled()) {
            throw new EdgeTransportException("边端 MQTT 原生通道未启用", null, false);
        }
        if (controlReady()) return;
        synchronized (connectionLock) {
            if (controlReady()) return;
            try {
                ensureChannelConnected(Channel.CONTROL);
            } catch (MqttException exception) {
                throw new EdgeTransportException("无法连接边端 MQTT 控制通道", exception, false);
            }
        }
    }

    private void ensureChannelConnected(Channel channel) throws MqttException {
        MqttAsyncClient current = client(channel);
        if (current == null) {
            current = clientFactory.create(properties.broker(), clientId(channel));
            current.setCallback(new ChannelCallback(channel, current));
            setClient(channel, current);
        }
        if (!current.isConnected()) {
            setSubscribed(channel, false);
            current.connect(connectOptions()).waitForCompletion(
                Duration.ofSeconds(properties.connectionTimeoutSeconds() + 5L).toMillis());
        }
        subscribe(channel, current);
    }

    private MqttConnectOptions connectOptions() {
        MqttConnectOptions options = new MqttConnectOptions();
        options.setAutomaticReconnect(true);
        options.setCleanSession(properties.cleanSession());
        options.setConnectionTimeout(properties.connectionTimeoutSeconds());
        options.setKeepAliveInterval(properties.keepAliveSeconds());
        options.setMaxInflight(200);
        if (properties.broker().startsWith("ssl://")) {
            options.setHttpsHostnameVerificationEnabled(true);
            if (properties.tls().customSocketFactoryRequired()) {
                options.setSocketFactory(PemTlsSocketFactory.create(properties.tls().caFile(),
                    properties.tls().clientCertificateFile(), properties.tls().clientPrivateKeyFile()));
            }
        } else if (properties.tls().customSocketFactoryRequired()) {
            throw new EdgeTransportException(
                "边端 MQTT TLS 文件只能用于 ssl:// Broker", null, false);
        }
        if (!properties.username().isBlank()) options.setUserName(properties.username());
        if (!properties.password().isBlank()) options.setPassword(properties.password().toCharArray());
        return options;
    }

    private synchronized void subscribe(Channel channel, MqttAsyncClient current) throws MqttException {
        if (!current.isConnected() || subscribed(channel)) return;
        setSubscribed(channel, false);
        List<String> topics = scopedTopics(channel == Channel.CONTROL ? CONTROL_TOPICS : DATA_TOPICS);
        List<String> staleTopics = scopedTopics(channel == Channel.CONTROL ? DATA_TOPICS : CONTROL_TOPICS);
        int qos = channel == Channel.CONTROL ? properties.controlQos() : properties.qos();
        int[] qosLevels = new int[topics.size()];
        java.util.Arrays.fill(qosLevels, qos);
        current.unsubscribe(staleTopics.toArray(String[]::new))
            .waitForCompletion(Duration.ofSeconds(5).toMillis());
        IMqttToken subscription = current.subscribe(topics.toArray(String[]::new), qosLevels);
        subscription.waitForCompletion(Duration.ofSeconds(5).toMillis());
        int[] grantedQos = subscription.getGrantedQos();
        if (grantedQos == null || grantedQos.length != topics.size()
            || java.util.Arrays.stream(grantedQos).anyMatch(value -> value != qos)) {
            throw new MqttException(MqttException.REASON_CODE_SUBSCRIBE_FAILED);
        }
        setSubscribed(channel, true);
    }

    private void connectComplete(Channel channel, MqttAsyncClient current, boolean reconnect) {
        if (client(channel) == current) setSubscribed(channel, false);
        log.info("Native edge MQTT {} channel connected; reconnect={}", channel.label, reconnect);
    }

    private void connectionLost(Channel channel, MqttAsyncClient current, Throwable cause) {
        if (client(channel) == current) setSubscribed(channel, false);
        log.warn("Native edge MQTT {} channel connection lost; failure={}",
            channel.label, failureCategory(cause));
    }

    private void messageArrived(String topic, MqttMessage message) {
        if (!properties.machineCode().isEmpty()
            && !scopedTopics(DATA_TOPICS).contains(topic) && !scopedTopics(CONTROL_TOPICS).contains(topic)) {
            logRejected(topic, EdgeMessageRejectedException.Reason.DEVICE_IDENTITY_MISMATCH);
            return;
        }
        byte[] bytes = message == null ? null : message.getPayload();
        if (bytes == null) {
            logRejected(topic, EdgeMessageRejectedException.Reason.EMPTY_PAYLOAD);
            return;
        }
        if (bytes.length > properties.inboundPayloadLimit(topic)) {
            logRejected(topic, EdgeMessageRejectedException.Reason.PAYLOAD_SIZE);
            return;
        }
        try {
            ingress.ingest(topic, new String(bytes, java.nio.charset.StandardCharsets.UTF_8));
        } catch (EdgeMessageRejectedException exception) {
            logRejected(topic, exception.reason());
        } catch (RuntimeException exception) {
            long count = processingFailureCount.incrementAndGet();
            if (shouldLog(count)) {
                log.error("Native edge MQTT ingestion failed; topic={}, reason=PROCESSING_FAILURE, "
                    + "failureType={}, totalOccurrences={}",
                    topic, exception.getClass().getSimpleName(), count);
            }
            // Paho must observe processing failures instead of acknowledging consumption.
            throw exception;
        }
    }

    private void logRejected(String topic, EdgeMessageRejectedException.Reason reason) {
        long count = rejectionCounts.computeIfAbsent(reason, ignored -> new AtomicLong()).incrementAndGet();
        if (shouldLog(count)) {
            log.warn("Native edge MQTT message rejected; topic={}, reason={}, totalOccurrences={}",
                topic, reason, count);
        }
    }

    private boolean shouldLog(long count) {
        return count == 1L || count % 100L == 0L;
    }

    @PreDestroy
    public void stop() {
        synchronized (connectionLock) {
            close(controlClient);
            close(dataClient);
            controlClient = null;
            dataClient = null;
            controlSubscribed = false;
            dataSubscribed = false;
        }
    }

    private void invalidate(Channel channel, MqttAsyncClient failedClient) {
        synchronized (connectionLock) {
            if (client(channel) != failedClient) return;
            setClient(channel, null);
            setSubscribed(channel, false);
            close(failedClient);
        }
    }

    private void close(MqttAsyncClient current) {
        if (current == null) return;
        try {
            if (current.isConnected()) current.disconnectForcibly(1000, 1000, false);
            current.close();
        } catch (MqttException ignored) {
        }
    }

    private boolean controlReady() {
        MqttAsyncClient current = controlClient;
        return current != null && current.isConnected() && controlSubscribed;
    }

    private boolean dataReady() {
        MqttAsyncClient current = dataClient;
        return current != null && current.isConnected() && dataSubscribed;
    }

    private MqttAsyncClient client(Channel channel) {
        return channel == Channel.CONTROL ? controlClient : dataClient;
    }

    private void setClient(Channel channel, MqttAsyncClient value) {
        if (channel == Channel.CONTROL) controlClient = value;
        else dataClient = value;
    }

    private boolean subscribed(Channel channel) {
        return channel == Channel.CONTROL ? controlSubscribed : dataSubscribed;
    }

    private void setSubscribed(Channel channel, boolean value) {
        if (channel == Channel.CONTROL) controlSubscribed = value;
        else dataSubscribed = value;
    }

    private String clientId(Channel channel) {
        return properties.clientId() + (channel == Channel.CONTROL ? "-control" : "-data");
    }

    private List<String> scopedTopics(List<String> topics) {
        if (properties.machineCode().isEmpty()) return topics;
        return topics.stream().map(topic -> topic.substring(0, topic.length() - 1)
            + properties.machineCode()).toList();
    }

    private void validate(String topic, byte[] payload, int qos) {
        if (topic == null || topic.isBlank() || topic.length() > 512 || topic.contains("\u0000")
            || topic.contains("+") || topic.contains("#")) {
            throw new IllegalArgumentException("边端 MQTT topic 非法");
        }
        if (payload != null && payload.length > properties.maxPayloadBytes()) {
            throw new IllegalArgumentException("边端 MQTT payload 超过大小限制");
        }
        if (!properties.machineCode().isEmpty() && !topic.endsWith("/" + properties.machineCode())) {
            throw new IllegalArgumentException("MQTT publish is outside the configured device scope");
        }
        if (qos < 0 || qos > 2) throw new IllegalArgumentException("边端 MQTT QoS 非法");
    }

    private String failureCategory(Throwable failure) {
        Throwable current = failure;
        String mqttReason = null;
        for (int depth = 0; current != null && depth < 8; depth++, current = current.getCause()) {
            if (current instanceof MqttException mqtt) mqttReason = "MQTT_REASON_" + mqtt.getReasonCode();
            if (current instanceof javax.net.ssl.SSLException) return "TLS_HANDSHAKE";
        }
        return mqttReason == null ? "CONNECTION_FAILED" : mqttReason;
    }

    @FunctionalInterface
    interface MqttClientFactory {
        MqttAsyncClient create(String broker, String clientId) throws MqttException;
    }

    private enum Channel {
        CONTROL("control"), DATA("data");

        private final String label;

        Channel(String label) {
            this.label = label;
        }
    }

    private final class ChannelCallback implements MqttCallbackExtended {
        private final Channel channel;
        private final MqttAsyncClient client;

        private ChannelCallback(Channel channel, MqttAsyncClient client) {
            this.channel = channel;
            this.client = client;
        }

        @Override
        public void connectComplete(boolean reconnect, String serverURI) {
            PahoEdgeCommandTransport.this.connectComplete(channel, client, reconnect);
        }

        @Override
        public void connectionLost(Throwable cause) {
            PahoEdgeCommandTransport.this.connectionLost(channel, client, cause);
        }

        @Override
        public void messageArrived(String topic, MqttMessage message) {
            PahoEdgeCommandTransport.this.messageArrived(topic, message);
        }

        @Override
        public void deliveryComplete(IMqttDeliveryToken token) {
        }
    }
}
