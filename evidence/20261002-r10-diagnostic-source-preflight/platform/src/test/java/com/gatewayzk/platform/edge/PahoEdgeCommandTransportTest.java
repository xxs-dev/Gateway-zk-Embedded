package com.gatewayzk.platform.edge;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.gatewayzk.platform.alarm.AlarmApplicationService;
import com.gatewayzk.platform.configrev.ConfigDeploymentStateService;
import com.gatewayzk.platform.task.DiagnosticResultSanitizer;
import org.eclipse.paho.client.mqttv3.IMqttDeliveryToken;
import org.eclipse.paho.client.mqttv3.IMqttToken;
import org.eclipse.paho.client.mqttv3.MqttAsyncClient;
import org.eclipse.paho.client.mqttv3.MqttCallbackExtended;
import org.eclipse.paho.client.mqttv3.MqttException;
import org.eclipse.paho.client.mqttv3.MqttMessage;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.Arguments;
import org.junit.jupiter.params.provider.MethodSource;
import org.junit.jupiter.params.provider.ValueSource;
import org.mockito.ArgumentCaptor;
import org.springframework.dao.DataAccessResourceFailureException;

import java.nio.charset.StandardCharsets;
import java.io.IOException;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.stream.Stream;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.mockito.ArgumentMatchers.anyLong;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.eq;
import static org.mockito.Mockito.atMostOnce;
import static org.mockito.Mockito.doAnswer;
import static org.mockito.Mockito.doThrow;
import static org.mockito.Mockito.mock;
import static org.mockito.Mockito.never;
import static org.mockito.Mockito.spy;
import static org.mockito.Mockito.times;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.verifyNoInteractions;
import static org.mockito.Mockito.verifyNoMoreInteractions;
import static org.mockito.Mockito.when;

class PahoEdgeCommandTransportTest {
    @Test
    void largerDiagnosticReplyUsesOnlyItsBoundedReceiveBudget() throws Exception {
        EdgeMessageIngestService ingress = mock(EdgeMessageIngestService.class);
        byte[] payload = new byte[4 * 1024 * 1024 + 1];
        java.util.Arrays.fill(payload, (byte) 'x');
        String diagnosticTopic = "edge/system/diag/reply/COMM_TEST";
        callbackForTopic(ingress, diagnosticTopic).messageArrived(diagnosticTopic, new MqttMessage(payload));
        verify(ingress).ingest(eq(diagnosticTopic), any(String.class));

        EdgeMessageIngestService ordinaryIngress = mock(EdgeMessageIngestService.class);
        String ordinaryTopic = "edge/status/COMM_TEST";
        dataCallback(ordinaryIngress).messageArrived(ordinaryTopic, new MqttMessage(payload));
        verifyNoInteractions(ordinaryIngress);
        assertTrue(properties().inboundPayloadLimit(diagnosticTopic) >= payload.length);
        assertTrue(properties().inboundPayloadLimit(ordinaryTopic) < payload.length);
    }
    @Test
    void databaseIngestionFailureEscapesCallbackInsteadOfReportingConsumption() throws Exception {
        EdgeMessageIngestService ingress = mock(EdgeMessageIngestService.class);
        String topic = "edge/telemetry/full/COMM_TEST";
        String payload = "{\"machineCode\":\"COMM_TEST\"}";
        doThrow(new DataAccessResourceFailureException("database unavailable"))
            .when(ingress).ingest(topic, payload);
        MqttCallbackExtended callback = dataCallback(ingress);

        assertThrows(DataAccessResourceFailureException.class,
            () -> callback.messageArrived(topic,
                new MqttMessage(payload.getBytes(StandardCharsets.UTF_8))));
    }

    @Test
    void permanentlyInvalidMessageDoesNotFailTheMqttCallback() throws Exception {
        EdgeMessageIngestService ingress = mock(EdgeMessageIngestService.class);
        String topic = "edge/telemetry/full/COMM_UNKNOWN";
        String payload = "{\"machineCode\":\"COMM_UNKNOWN\"}";
        doThrow(new EdgeMessageRejectedException(
            EdgeMessageRejectedException.Reason.UNREGISTERED_DEVICE, "未登记的边端设备"))
            .when(ingress).ingest(topic, payload);
        MqttCallbackExtended callback = dataCallback(ingress);

        assertDoesNotThrow(() -> callback.messageArrived(topic,
            new MqttMessage(payload.getBytes(StandardCharsets.UTF_8))));
    }

    @Test
    void programmingFailureEscapesCallbackUnchanged() throws Exception {
        EdgeMessageIngestService ingress = mock(EdgeMessageIngestService.class);
        IllegalArgumentException failure = new IllegalArgumentException("invalid repository argument");
        doThrow(failure).when(ingress).ingest("edge/status/COMM_TEST", "{}");
        MqttCallbackExtended callback = dataCallback(ingress);

        assertSame(failure, assertThrows(IllegalArgumentException.class,
            () -> callback.messageArrived("edge/status/COMM_TEST", message("{}"))));
    }

    @Test
    void successfulCallbackPersistsRealtimePointsBeforeReturning() throws Exception {
        EdgeRuntimeRepository repository = mock(EdgeRuntimeRepository.class);
        when(repository.findDeviceId("COMM_TEST")).thenReturn(Optional.of(42L));
        MqttCallbackExtended callback = dataCallback(realIngress(repository));

        assertDoesNotThrow(() -> callback.messageArrived("edge/telemetry/full/COMM_TEST",
            message(realtimePayload())));

        verify(repository).upsertPoints(org.mockito.ArgumentMatchers.argThat(points ->
            points.size() == 1 && points.getFirst().deviceId() == 42L
                && points.getFirst().pointIndex() == 1), eq(true));
        verify(repository).recordSeen(eq(42L), any());
    }

    @ParameterizedTest
    @ValueSource(strings = {"", " \n\t", "null", "[]", "[1]", "42", "true", "\"text\"", "{"})
    void invalidPayloadReturnsFromRealCallbackWithoutDatabaseAccess(String payload) throws Exception {
        EdgeRuntimeRepository repository = mock(EdgeRuntimeRepository.class);
        MqttCallbackExtended callback = dataCallback(realIngress(repository));

        assertDoesNotThrow(() -> callback.messageArrived("edge/status/COMM_TEST", message(payload)));

        verifyNoInteractions(repository);
    }

    @Test
    void repositoryWriteFailureEscapesRealIngestionCallbackUnchanged() throws Exception {
        EdgeRuntimeRepository repository = mock(EdgeRuntimeRepository.class);
        when(repository.findDeviceId("COMM_TEST")).thenReturn(Optional.of(42L));
        DataAccessResourceFailureException failure =
            new DataAccessResourceFailureException("database unavailable");
        doThrow(failure).when(repository).upsertPoints(any(), eq(true));
        MqttCallbackExtended callback = dataCallback(realIngress(repository));

        assertSame(failure, assertThrows(DataAccessResourceFailureException.class,
            () -> callback.messageArrived("edge/telemetry/full/COMM_TEST", message(realtimePayload()))));

        verify(repository).upsertPoints(any(), eq(true));
        verify(repository, never()).recordSeen(anyLong(), any());
    }

    @ParameterizedTest(name = "{0}")
    @MethodSource("invalidTopicPayloads")
    void explicitPayloadViolationsAreConsumedWithoutWrites(String topic, String payload) throws Exception {
        EdgeRuntimeRepository repository = mock(EdgeRuntimeRepository.class);
        when(repository.findDeviceId("COMM_TEST")).thenReturn(Optional.of(42L));
        when(repository.activeRealtimeLeaseForUpdate(eq("REALTIME_SELECTOR"), eq(42L), any()))
            .thenReturn(Optional.of(new EdgeRuntimeRepository.RealtimeLease("REALTIME_SELECTOR", 42L,
                "COMM_TEST", "PCS_SELECTED", Instant.MAX, "ACTIVE", Instant.EPOCH, null)));
        MqttCallbackExtended callback = callbackForTopic(realIngress(repository), topic);

        assertDoesNotThrow(() -> callback.messageArrived(topic, message(payload)));

        verify(repository, atMostOnce()).findDeviceId("COMM_TEST");
        verify(repository, atMostOnce()).activeRealtimeLeaseForUpdate(any(), eq(42L), any());
        verifyNoMoreInteractions(repository);
    }

    private static Stream<Arguments> invalidTopicPayloads() {
        String identity = "{\"machineCode\":\"COMM_TEST\"}";
        String configTopic = "edge/config/pull/reply/COMM_TEST";
        String realtimeTopic = "edge/telemetry/realtime/COMM_TEST";
        var tooManyFiles = new ObjectMapper().createObjectNode();
        tooManyFiles.put("machineCode", "COMM_TEST").put("requestId", "REQUEST_TEST");
        var files = tooManyFiles.putArray("files");
        for (int index = 0; index < 257; index++) files.addObject();
        return Stream.of(
            Arguments.of(configTopic, identity),
            Arguments.of("edge/camera/status/COMM_TEST", identity),
            Arguments.of("edge/system/diag/reply/COMM_TEST", identity),
            Arguments.of("edge/alarm/COMM_TEST", identity),
            Arguments.of("edge/system/monitor/alert/COMM_TEST", identity),
            Arguments.of("edge/system/monitor/alert/COMM_TEST",
                "{\"machineCode\":\"COMM_TEST\",\"metric\":\"CPU\"}"),
            Arguments.of("edge/status/COMM_TEST", "{\"machineCode\":\"COMM_OTHER\"}"),
            Arguments.of(realtimeTopic, "{\"machineCode\":\"COMM_TEST\",\"sessionId\":null}"),
            Arguments.of(realtimeTopic, "{\"machineCode\":\"COMM_TEST\",\"sessionId\":\"bad\"}"),
            Arguments.of(realtimeTopic, "{\"machineCode\":\"COMM_TEST\",\"sessionId\":\"REALTIME_EXPIRED\"}"),
            Arguments.of(realtimeTopic, """
                {"machineCode":"COMM_TEST","sessionId":"REALTIME_SELECTOR",
                 "meters":[{"meterCode":"PCS_OTHER","values":[]}]}
                """),
            Arguments.of(configTopic, "{\"machineCode\":\"COMM_TEST\",\"chunked\":true}"),
            Arguments.of(configTopic, chunk("REQUEST_TEST", 1, 1, 2, "")),
            Arguments.of(configTopic, """
                {"machineCode":"COMM_TEST","requestId":"REQUEST_TEST","chunked":true,
                 "chunkIndex":1,"chunkCount":1,"totalBytes":2,"payloadHex":"XYZ"}
                """),
            Arguments.of(configTopic, chunk("REQUEST_TEST", 1, 1, 3, "{}")),
            Arguments.of(configTopic, chunk("REQUEST_TEST", 1, 1, 2, "{}")),
            Arguments.of(configTopic, tooManyFiles.toString())
        );
    }

    @ParameterizedTest
    @ValueSource(strings = {" ", "null", "[]", "[1]", "42", "true", "\"text\"", "{"})
    void invalidAssembledJsonIsConsumedWithoutDatabaseAccess(String assembled) throws Exception {
        EdgeRuntimeRepository repository = mock(EdgeRuntimeRepository.class);
        String topic = "edge/config/pull/reply/COMM_TEST";
        MqttCallbackExtended callback = callbackForTopic(realIngress(repository), topic);

        assertDoesNotThrow(() -> callback.messageArrived(topic,
            message(chunk("REQUEST_TEST", 1, 1, assembled.getBytes(StandardCharsets.UTF_8).length, assembled))));

        verifyNoInteractions(repository);
    }

    @Test
    void nullAssembledTreeIsConsumedWithoutDatabaseAccess() throws Exception {
        EdgeRuntimeRepository repository = mock(EdgeRuntimeRepository.class);
        ObjectMapper mapper = spy(new ObjectMapper());
        org.mockito.Mockito.doReturn(null).when(mapper).readTree(any(byte[].class));
        String topic = "edge/config/pull/reply/COMM_TEST";
        MqttCallbackExtended callback = callbackForTopic(realIngress(repository, mapper), topic);

        assertDoesNotThrow(() -> callback.messageArrived(topic,
            message(chunk("REQUEST_TEST", 1, 1, 2, "{}"))));

        verifyNoInteractions(repository);
    }

    @Test
    void conflictingChunkMetadataIsConsumedWithoutDatabaseAccess() throws Exception {
        EdgeRuntimeRepository repository = mock(EdgeRuntimeRepository.class);
        String topic = "edge/config/pull/reply/COMM_TEST";
        MqttCallbackExtended callback = callbackForTopic(realIngress(repository), topic);
        callback.messageArrived(topic, message(chunk("REQUEST_TEST", 1, 2, 2, "{")));

        assertDoesNotThrow(() -> callback.messageArrived(topic,
            message(chunk("REQUEST_TEST", 2, 2, 3, "}"))));

        verifyNoInteractions(repository);
    }

    @Test
    void temporaryAssemblyCapacityPressureStillEscapesCallback() throws Exception {
        EdgeRuntimeRepository repository = mock(EdgeRuntimeRepository.class);
        String topic = "edge/config/pull/reply/COMM_TEST";
        MqttCallbackExtended callback = callbackForTopic(realIngress(repository), topic);
        for (int index = 0; index < 256; index++) {
            callback.messageArrived(topic, message(chunk("REQUEST_" + index, 1, 2, 2, "{")));
        }

        IllegalArgumentException failure = assertThrows(IllegalArgumentException.class,
            () -> callback.messageArrived(topic, message(chunk("REQUEST_OVERFLOW", 1, 2, 2, "{"))));

        assertFalse(failure instanceof EdgeMessageRejectedException);
        verifyNoInteractions(repository);
    }

    @Test
    void unknownRepositoryLookupFailureStillEscapesCallbackUnchanged() throws Exception {
        EdgeRuntimeRepository repository = mock(EdgeRuntimeRepository.class);
        IllegalArgumentException failure = new IllegalArgumentException("repository configuration failure");
        when(repository.findDeviceId("COMM_TEST")).thenThrow(failure);
        MqttCallbackExtended callback = dataCallback(realIngress(repository));

        assertSame(failure, assertThrows(IllegalArgumentException.class,
            () -> callback.messageArrived("edge/status/COMM_TEST", message("{}"))));
    }

    @Test
    void unknownLeaseLookupFailureStillEscapesCallbackUnchanged() throws Exception {
        EdgeRuntimeRepository repository = mock(EdgeRuntimeRepository.class);
        when(repository.findDeviceId("COMM_TEST")).thenReturn(Optional.of(42L));
        DataAccessResourceFailureException failure = new DataAccessResourceFailureException("database unavailable");
        when(repository.activeRealtimeLeaseForUpdate(eq("REALTIME_TEST"), eq(42L), any())).thenThrow(failure);
        MqttCallbackExtended callback = dataCallback(realIngress(repository));

        assertSame(failure, assertThrows(DataAccessResourceFailureException.class,
            () -> callback.messageArrived("edge/telemetry/realtime/COMM_TEST",
                message("{\"machineCode\":\"COMM_TEST\",\"sessionId\":\"REALTIME_TEST\"}"))));

        verify(repository, never()).upsertPoints(any(), org.mockito.ArgumentMatchers.anyBoolean());
    }

    @Test
    void assembledMapperRuntimeFailureStillEscapesCallbackUnchanged() throws Exception {
        EdgeRuntimeRepository repository = mock(EdgeRuntimeRepository.class);
        ObjectMapper mapper = spy(new ObjectMapper());
        IllegalArgumentException failure = new IllegalArgumentException("mapper configuration failure");
        doThrow(failure).when(mapper).readTree(any(byte[].class));
        String topic = "edge/config/pull/reply/COMM_TEST";
        MqttCallbackExtended callback = callbackForTopic(realIngress(repository, mapper), topic);

        assertSame(failure, assertThrows(IllegalArgumentException.class,
            () -> callback.messageArrived(topic, message(chunk("REQUEST_TEST", 1, 1, 2, "{}")))));

        verifyNoInteractions(repository);
    }

    @Test
    void assembledMapperIoFailureIsNotClassifiedAsPayloadRejection() throws Exception {
        EdgeRuntimeRepository repository = mock(EdgeRuntimeRepository.class);
        ObjectMapper mapper = spy(new ObjectMapper());
        IOException failure = new IOException("unexpected mapper IO failure");
        doThrow(failure).when(mapper).readTree(any(byte[].class));
        String topic = "edge/config/pull/reply/COMM_TEST";
        MqttCallbackExtended callback = callbackForTopic(realIngress(repository, mapper), topic);

        IllegalStateException propagated = assertThrows(IllegalStateException.class,
            () -> callback.messageArrived(topic, message(chunk("REQUEST_TEST", 1, 1, 2, "{}"))));

        assertSame(failure, propagated.getCause());
        verifyNoInteractions(repository);
    }

    private static String chunk(String requestId, int index, int count, long totalBytes, String part) {
        return new ObjectMapper().createObjectNode().put("machineCode", "COMM_TEST")
            .put("requestId", requestId).put("chunked", true).put("chunkIndex", index)
            .put("chunkCount", count).put("totalBytes", totalBytes).put("payloadPart", part).toString();
    }

    private EdgeMessageIngestService realIngress(EdgeRuntimeRepository repository) {
        return realIngress(repository, new ObjectMapper());
    }

    private EdgeMessageIngestService realIngress(EdgeRuntimeRepository repository, ObjectMapper mapper) {
        return new EdgeMessageIngestService(repository, mapper,
            mock(AlarmApplicationService.class), mock(DiagnosticResultSanitizer.class),
            mock(ConfigDeploymentStateService.class), properties());
    }

    private String realtimePayload() {
        return """
            {"machineCode":"COMM_TEST","meters":[{"meterCode":"PCS_1",
             "values":[{"index":1,"value":12.5,"ts":1780000000000}]}]}
            """;
    }

    private MqttMessage message(String payload) {
        MqttMessage message = new MqttMessage(payload.getBytes(StandardCharsets.UTF_8));
        message.setQos(1);
        return message;
    }

    @Test
    void telemetryIngestionCannotBlockControlPublishing() throws Exception {
        EdgeMessageIngestService ingress = mock(EdgeMessageIngestService.class);
        MqttAsyncClient controlClient = mock(MqttAsyncClient.class);
        MqttAsyncClient dataClient = mock(MqttAsyncClient.class);
        IMqttDeliveryToken delivery = mock(IMqttDeliveryToken.class);
        when(controlClient.isConnected()).thenReturn(true);
        when(dataClient.isConnected()).thenReturn(true);
        IMqttToken controlUnsubscribe = mock(IMqttToken.class);
        IMqttToken dataUnsubscribe = mock(IMqttToken.class);
        IMqttToken controlSubscribe = mock(IMqttToken.class);
        IMqttToken dataSubscribe = mock(IMqttToken.class);
        when(controlClient.unsubscribe(any(String[].class))).thenReturn(controlUnsubscribe);
        when(dataClient.unsubscribe(any(String[].class))).thenReturn(dataUnsubscribe);
        when(controlClient.subscribe(any(String[].class), any(int[].class))).thenReturn(controlSubscribe);
        when(dataClient.subscribe(any(String[].class), any(int[].class))).thenReturn(dataSubscribe);
        when(controlSubscribe.getGrantedQos()).thenReturn(new int[]{2, 2, 2, 2});
        when(dataSubscribe.getGrantedQos()).thenReturn(new int[]{1, 1, 1, 1, 1, 1, 1, 1, 1});
        when(controlClient.publish(eq("edge/config/pull/request/COMM_TEST"),
            org.mockito.ArgumentMatchers.any(MqttMessage.class))).thenReturn(delivery);

        List<String> clientIds = new ArrayList<>();
        PahoEdgeCommandTransport transport = new PahoEdgeCommandTransport(properties(), ingress,
            (broker, clientId) -> {
                clientIds.add(clientId);
                return clientId.endsWith("-data") ? dataClient : controlClient;
            });
        transport.start();

        assertTrue(clientIds.contains("platform-edge-control"));
        assertTrue(clientIds.contains("platform-edge-data"));
        verify(controlClient).unsubscribe(org.mockito.ArgumentMatchers.<String[]>argThat(
            topics -> java.util.List.of(topics).contains("edge/telemetry/full/+")));
        verify(dataClient).unsubscribe(org.mockito.ArgumentMatchers.<String[]>argThat(
            topics -> java.util.List.of(topics).contains("edge/system/monitor/telemetry/+")));
        verify(dataClient).unsubscribe(org.mockito.ArgumentMatchers.<String[]>argThat(
            topics -> java.util.List.of(topics).contains("edge/config/pull/reply/+")));
        verify(controlClient).subscribe(org.mockito.ArgumentMatchers.<String[]>argThat(
            topics -> java.util.List.of(topics).contains("edge/config/pull/reply/+")
                && java.util.List.of(topics).contains("edge/system/monitor/telemetry/+")),
            org.mockito.ArgumentMatchers.<int[]>argThat(qos ->
                java.util.Arrays.stream(qos).allMatch(value -> value == 2)));
        verify(dataClient).subscribe(org.mockito.ArgumentMatchers.<String[]>argThat(
            topics -> java.util.List.of(topics).contains("edge/telemetry/full/+")),
            org.mockito.ArgumentMatchers.<int[]>argThat(qos ->
                java.util.Arrays.stream(qos).allMatch(value -> value == 1)));
        verify(controlUnsubscribe).waitForCompletion(anyLong());
        verify(dataUnsubscribe).waitForCompletion(anyLong());
        verify(controlSubscribe).waitForCompletion(anyLong());
        verify(dataSubscribe).waitForCompletion(anyLong());

        ArgumentCaptor<MqttCallbackExtended> callback = ArgumentCaptor.forClass(MqttCallbackExtended.class);
        verify(dataClient).setCallback(callback.capture());
        CountDownLatch ingestStarted = new CountDownLatch(1);
        CountDownLatch releaseIngest = new CountDownLatch(1);
        doAnswer(invocation -> {
            ingestStarted.countDown();
            releaseIngest.await(5, TimeUnit.SECONDS);
            return null;
        }).when(ingress).ingest(eq("edge/telemetry/full/COMM_TEST"),
            eq("{\"machineCode\":\"COMM_TEST\"}"));

        Thread dataCallback = new Thread(() -> {
            try {
                callback.getValue().messageArrived("edge/telemetry/full/COMM_TEST",
                    new MqttMessage("{\"machineCode\":\"COMM_TEST\"}"
                        .getBytes(StandardCharsets.UTF_8)));
            } catch (Exception exception) {
                throw new RuntimeException(exception);
            }
        });
        dataCallback.start();
        assertTrue(ingestStarted.await(2, TimeUnit.SECONDS));

        try {
            transport.publish("edge/config/pull/request/COMM_TEST", new byte[]{1}, 2);
            verify(controlClient).publish(eq("edge/config/pull/request/COMM_TEST"),
                org.mockito.ArgumentMatchers.any(MqttMessage.class));
            verify(delivery).waitForCompletion(anyLong());
            verify(dataClient, never()).publish(eq("edge/config/pull/request/COMM_TEST"),
                org.mockito.ArgumentMatchers.any(MqttMessage.class));
        } finally {
            releaseIngest.countDown();
            dataCallback.join(2000);
        }
    }

    @Test
    void synchronousPublishFailureIsDefinitelyNotDelivered() throws Exception {
        MqttAsyncClient controlClient = mock(MqttAsyncClient.class);
        MqttAsyncClient dataClient = mock(MqttAsyncClient.class);
        configureConnectedClients(controlClient, dataClient);
        when(controlClient.publish(eq("edge/telemetry/realtime/request/COMM_TEST"),
            any(MqttMessage.class))).thenThrow(new MqttException(MqttException.REASON_CODE_CLIENT_NOT_CONNECTED));
        PahoEdgeCommandTransport transport = startTransport(controlClient, dataClient);

        EdgeTransportException failure = assertThrows(EdgeTransportException.class,
            () -> transport.publish("edge/telemetry/realtime/request/COMM_TEST", new byte[]{1}, 2));

        assertFalse(failure.deliveryUncertain());
    }

    @Test
    void publishAcknowledgementFailureRemainsDeliveryUncertain() throws Exception {
        MqttAsyncClient controlClient = mock(MqttAsyncClient.class);
        MqttAsyncClient dataClient = mock(MqttAsyncClient.class);
        configureConnectedClients(controlClient, dataClient);
        IMqttDeliveryToken delivery = mock(IMqttDeliveryToken.class);
        when(controlClient.publish(eq("edge/telemetry/realtime/request/COMM_TEST"),
            any(MqttMessage.class))).thenReturn(delivery);
        doThrow(new MqttException(MqttException.REASON_CODE_CLIENT_TIMEOUT))
            .when(delivery).waitForCompletion(anyLong());
        PahoEdgeCommandTransport transport = startTransport(controlClient, dataClient);

        EdgeTransportException failure = assertThrows(EdgeTransportException.class,
            () -> transport.publish("edge/telemetry/realtime/request/COMM_TEST", new byte[]{1}, 2));

        assertTrue(failure.deliveryUncertain());
    }

    @Test
    void dataSubscriptionTimeoutStaysNotReadyAndMaintenanceRetries() throws Exception {
        MqttAsyncClient controlClient = mock(MqttAsyncClient.class);
        MqttAsyncClient dataClient = mock(MqttAsyncClient.class);
        when(controlClient.isConnected()).thenReturn(true);
        when(dataClient.isConnected()).thenReturn(true);
        configureSuccessfulSubscription(controlClient, new int[]{2, 2, 2, 2});
        IMqttToken dataUnsubscribe = mock(IMqttToken.class);
        IMqttToken timedOutSubscribe = mock(IMqttToken.class);
        IMqttToken successfulSubscribe = mock(IMqttToken.class);
        when(dataClient.unsubscribe(any(String[].class))).thenReturn(dataUnsubscribe);
        when(dataClient.subscribe(any(String[].class), any(int[].class)))
            .thenReturn(timedOutSubscribe, successfulSubscribe);
        doThrow(new MqttException(MqttException.REASON_CODE_CLIENT_TIMEOUT))
            .when(timedOutSubscribe).waitForCompletion(anyLong());
        when(successfulSubscribe.getGrantedQos())
            .thenReturn(new int[]{1, 1, 1, 1, 1, 1, 1, 1, 1});
        PahoEdgeCommandTransport transport = startTransport(controlClient, dataClient);

        transport.maintainConnection();
        transport.maintainConnection();

        verify(dataClient, times(2)).subscribe(any(String[].class), any(int[].class));
        verify(successfulSubscribe).waitForCompletion(anyLong());
    }

    @Test
    void dataUnsubscribeTimeoutStaysNotReadyAndMaintenanceRetries() throws Exception {
        MqttAsyncClient controlClient = mock(MqttAsyncClient.class);
        MqttAsyncClient dataClient = mock(MqttAsyncClient.class);
        when(controlClient.isConnected()).thenReturn(true);
        when(dataClient.isConnected()).thenReturn(true);
        configureSuccessfulSubscription(controlClient, new int[]{2, 2, 2, 2});
        IMqttToken timedOutUnsubscribe = mock(IMqttToken.class);
        IMqttToken successfulUnsubscribe = mock(IMqttToken.class);
        IMqttToken successfulSubscribe = mock(IMqttToken.class);
        when(dataClient.unsubscribe(any(String[].class)))
            .thenReturn(timedOutUnsubscribe, successfulUnsubscribe);
        doThrow(new MqttException(MqttException.REASON_CODE_CLIENT_TIMEOUT))
            .when(timedOutUnsubscribe).waitForCompletion(anyLong());
        when(dataClient.subscribe(any(String[].class), any(int[].class)))
            .thenReturn(successfulSubscribe);
        when(successfulSubscribe.getGrantedQos())
            .thenReturn(new int[]{1, 1, 1, 1, 1, 1, 1, 1, 1});
        PahoEdgeCommandTransport transport = startTransport(controlClient, dataClient);

        transport.maintainConnection();

        verify(dataClient, times(2)).unsubscribe(any(String[].class));
        verify(successfulUnsubscribe).waitForCompletion(anyLong());
        verify(dataClient).subscribe(any(String[].class), any(int[].class));
    }

    @Test
    void rejectedDataSubscriptionStaysNotReadyAndMaintenanceRetries() throws Exception {
        MqttAsyncClient controlClient = mock(MqttAsyncClient.class);
        MqttAsyncClient dataClient = mock(MqttAsyncClient.class);
        when(controlClient.isConnected()).thenReturn(true);
        when(dataClient.isConnected()).thenReturn(true);
        configureSuccessfulSubscription(controlClient, new int[]{2, 2, 2, 2});
        IMqttToken dataUnsubscribe = mock(IMqttToken.class);
        IMqttToken rejectedSubscribe = mock(IMqttToken.class);
        IMqttToken successfulSubscribe = mock(IMqttToken.class);
        when(dataClient.unsubscribe(any(String[].class))).thenReturn(dataUnsubscribe);
        when(dataClient.subscribe(any(String[].class), any(int[].class)))
            .thenReturn(rejectedSubscribe, successfulSubscribe);
        when(rejectedSubscribe.getGrantedQos())
            .thenReturn(new int[]{1, 1, 1, 1, 128, 1, 1, 1, 1});
        when(successfulSubscribe.getGrantedQos())
            .thenReturn(new int[]{1, 1, 1, 1, 1, 1, 1, 1, 1});
        PahoEdgeCommandTransport transport = startTransport(controlClient, dataClient);

        transport.maintainConnection();
        transport.maintainConnection();

        verify(dataClient, times(2)).subscribe(any(String[].class), any(int[].class));
    }

    @Test
    void downgradedControlSubscriptionStaysNotReadyAndMaintenanceRetries() throws Exception {
        MqttAsyncClient controlClient = mock(MqttAsyncClient.class);
        MqttAsyncClient dataClient = mock(MqttAsyncClient.class);
        when(controlClient.isConnected()).thenReturn(true);
        when(dataClient.isConnected()).thenReturn(true);
        IMqttToken controlUnsubscribe = mock(IMqttToken.class);
        IMqttToken downgradedSubscribe = mock(IMqttToken.class);
        IMqttToken successfulSubscribe = mock(IMqttToken.class);
        when(controlClient.unsubscribe(any(String[].class))).thenReturn(controlUnsubscribe);
        when(controlClient.subscribe(any(String[].class), any(int[].class)))
            .thenReturn(downgradedSubscribe, successfulSubscribe);
        when(downgradedSubscribe.getGrantedQos()).thenReturn(new int[]{2, 2, 1, 2});
        when(successfulSubscribe.getGrantedQos()).thenReturn(new int[]{2, 2, 2, 2});
        configureSuccessfulSubscription(dataClient, new int[]{1, 1, 1, 1, 1, 1, 1, 1, 1});
        PahoEdgeCommandTransport transport = startTransport(controlClient, dataClient);

        transport.maintainConnection();
        transport.maintainConnection();

        verify(controlClient, times(2)).subscribe(any(String[].class), any(int[].class));
    }

    @Test
    void downgradedDataSubscriptionStaysNotReadyAndMaintenanceRetries() throws Exception {
        MqttAsyncClient controlClient = mock(MqttAsyncClient.class);
        MqttAsyncClient dataClient = mock(MqttAsyncClient.class);
        when(controlClient.isConnected()).thenReturn(true);
        when(dataClient.isConnected()).thenReturn(true);
        configureSuccessfulSubscription(controlClient, new int[]{2, 2, 2, 2});
        IMqttToken dataUnsubscribe = mock(IMqttToken.class);
        IMqttToken downgradedSubscribe = mock(IMqttToken.class);
        IMqttToken successfulSubscribe = mock(IMqttToken.class);
        when(dataClient.unsubscribe(any(String[].class))).thenReturn(dataUnsubscribe);
        when(dataClient.subscribe(any(String[].class), any(int[].class)))
            .thenReturn(downgradedSubscribe, successfulSubscribe);
        when(downgradedSubscribe.getGrantedQos())
            .thenReturn(new int[]{1, 1, 1, 1, 0, 1, 1, 1, 1});
        when(successfulSubscribe.getGrantedQos())
            .thenReturn(new int[]{1, 1, 1, 1, 1, 1, 1, 1, 1});
        PahoEdgeCommandTransport transport = startTransport(controlClient, dataClient);

        transport.maintainConnection();
        transport.maintainConnection();

        verify(dataClient, times(2)).subscribe(any(String[].class), any(int[].class));
    }

    private PahoEdgeCommandTransport startTransport(MqttAsyncClient controlClient,
                                                     MqttAsyncClient dataClient) {
        PahoEdgeCommandTransport transport = new PahoEdgeCommandTransport(
            properties(), mock(EdgeMessageIngestService.class),
            (broker, clientId) -> clientId.endsWith("-data") ? dataClient : controlClient);
        transport.start();
        return transport;
    }

    private MqttCallbackExtended dataCallback(EdgeMessageIngestService ingress) throws Exception {
        return registeredCallback(ingress, false);
    }

    private MqttCallbackExtended callbackForTopic(EdgeMessageIngestService ingress, String topic) throws Exception {
        return registeredCallback(ingress, topic.startsWith("edge/config/pull/reply/")
            || topic.startsWith("edge/system/diag/reply/"));
    }

    private MqttCallbackExtended registeredCallback(EdgeMessageIngestService ingress,
                                                   boolean controlChannel) throws Exception {
        MqttAsyncClient controlClient = mock(MqttAsyncClient.class);
        MqttAsyncClient dataClient = mock(MqttAsyncClient.class);
        configureConnectedClients(controlClient, dataClient);
        PahoEdgeCommandTransport transport = new PahoEdgeCommandTransport(
            properties(), ingress,
            (broker, clientId) -> clientId.endsWith("-data") ? dataClient : controlClient);
        transport.start();
        ArgumentCaptor<MqttCallbackExtended> callback =
            ArgumentCaptor.forClass(MqttCallbackExtended.class);
        verify(controlChannel ? controlClient : dataClient).setCallback(callback.capture());
        return callback.getValue();
    }

    private void configureConnectedClients(MqttAsyncClient controlClient,
                                           MqttAsyncClient dataClient) throws MqttException {
        when(controlClient.isConnected()).thenReturn(true);
        when(dataClient.isConnected()).thenReturn(true);
        configureSuccessfulSubscription(controlClient, new int[]{2, 2, 2, 2});
        configureSuccessfulSubscription(dataClient, new int[]{1, 1, 1, 1, 1, 1, 1, 1, 1});
    }

    private void configureSuccessfulSubscription(MqttAsyncClient client,
                                                 int[] grantedQos) throws MqttException {
        IMqttToken unsubscribe = mock(IMqttToken.class);
        IMqttToken subscribe = mock(IMqttToken.class);
        when(client.unsubscribe(any(String[].class))).thenReturn(unsubscribe);
        when(client.subscribe(any(String[].class), any(int[].class))).thenReturn(subscribe);
        when(subscribe.getGrantedQos()).thenReturn(grantedQos);
    }

    private EdgeMqttProperties properties() {
        return new EdgeMqttProperties(true, "tcp://broker:1883", "platform-edge",
            "", "", true, 10, 30, 1, 2, 4 * 1024 * 1024,
            new EdgeMqttProperties.Tls("", "", ""), null);
    }
}
