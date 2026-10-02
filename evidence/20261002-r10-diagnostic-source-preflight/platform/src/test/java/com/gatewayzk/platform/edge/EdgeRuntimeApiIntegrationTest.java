package com.gatewayzk.platform.edge;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.gatewayzk.platform.asset.AssetJdbcRepository;
import com.gatewayzk.platform.asset.DeviceCatalogService;
import com.gatewayzk.platform.audit.AuditService;
import com.gatewayzk.platform.identity.IdentityRepository;
import com.gatewayzk.platform.identity.PlatformPrincipal;
import com.gatewayzk.platform.task.DiagnosticResultSanitizer;
import com.gatewayzk.platform.task.TaskEngineProperties;
import com.gatewayzk.platform.task.TaskJdbcRepository;
import com.gatewayzk.platform.task.TaskLifecycleService;
import com.gatewayzk.platform.task.TaskWorker;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;
import org.springframework.aop.framework.ProxyFactory;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.boot.test.mock.mockito.SpyBean;
import org.springframework.security.test.context.support.WithUserDetails;
import org.springframework.security.core.Authentication;
import org.springframework.security.core.context.SecurityContextHolder;
import org.springframework.test.context.ActiveProfiles;
import org.springframework.boot.test.mock.mockito.MockBean;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.MvcResult;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.jdbc.core.namedparam.MapSqlParameterSource;
import org.springframework.jdbc.core.namedparam.NamedParameterJdbcTemplate;
import org.springframework.jdbc.datasource.DataSourceTransactionManager;
import org.springframework.transaction.annotation.AnnotationTransactionAttributeSource;
import org.springframework.transaction.interceptor.TransactionInterceptor;
import org.springframework.transaction.support.TransactionSynchronizationManager;
import org.springframework.transaction.support.TransactionTemplate;
import org.springframework.test.context.transaction.TestTransaction;
import org.springframework.transaction.annotation.Propagation;
import org.springframework.transaction.annotation.Transactional;

import java.util.ArrayList;
import java.util.List;
import java.sql.Connection;
import java.sql.Timestamp;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.UUID;
import java.time.Instant;
import java.util.concurrent.atomic.AtomicInteger;

import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.delete;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.csrf;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.user;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.content;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.header;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;
import static org.hamcrest.Matchers.not;
import static org.hamcrest.Matchers.containsString;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.mockito.Mockito.doAnswer;
import static org.mockito.Mockito.doThrow;
import static org.mockito.Mockito.when;
import static org.mockito.Mockito.verify;
import static org.mockito.Mockito.never;
import static org.mockito.Mockito.clearInvocations;
import static org.mockito.Mockito.spy;
import static org.mockito.Mockito.times;
import static org.mockito.ArgumentMatchers.any;
import static org.mockito.ArgumentMatchers.anyInt;
import static org.mockito.ArgumentMatchers.anyLong;
import static org.mockito.ArgumentMatchers.anyMap;
import static org.mockito.ArgumentMatchers.anyString;
import static org.mockito.ArgumentMatchers.argThat;
import static org.mockito.ArgumentMatchers.eq;

@SpringBootTest
@AutoConfigureMockMvc
@ActiveProfiles("test")
@Transactional
class EdgeRuntimeApiIntegrationTest {

    @Autowired private MockMvc mockMvc;
    @Autowired private AssetJdbcRepository assets;
    @Autowired private DeviceCatalogService devices;
    @Autowired private IdentityRepository identities;
    @Autowired private EdgeMessageIngestService ingress;
    @Autowired private TaskJdbcRepository tasks;
    @Autowired private TaskLifecycleService lifecycle;
    @Autowired private TaskEngineProperties taskProperties;
    @Autowired private DiagnosticResultSanitizer sanitizer;
    @Autowired private ObjectMapper objectMapper;
    @Autowired private EdgeOperationsGateway operations;
    @Autowired private EdgeRuntimeService runtime;
    @Autowired private JdbcTemplate jdbc;
    @SpyBean private EdgeRuntimeRepository edgeRuntimeRepository;
    @SpyBean private AuditService audit;
    @MockBean private EdgeCommandTransport transport;

    private String machineCode;
    private final List<Long> fixtureProjects = new ArrayList<>();
    private List<String> sessionsBefore;

    @BeforeEach
    void setup() {
        if (!TestTransaction.isActive()) {
            sessionsBefore = jdbc.queryForList("SELECT PRIMARY_ID FROM SPRING_SESSION", String.class);
        }
        String suffix = UUID.randomUUID().toString().substring(0, 8);
        long adminId = identities.findByUsername("admin").orElseThrow().id();
        long projectId = assets.createProject("EDGE_" + suffix, "原生接入项目", adminId);
        fixtureProjects.add(projectId);
        long siteId = assets.createSite(projectId, "EDGE_SITE_" + suffix,
            "原生接入站点", "Asia/Shanghai", adminId);
        machineCode = "GW-NATIVE-" + suffix;
        assets.createDevice(siteId, machineCode, "原生接入设备", "EDGE-GW", "MANUAL", adminId);
    }

    @AfterEach
    void cleanCommittedRealtimeFixtures() {
        if (TestTransaction.isActive()) return;
        // Only REQUIRES_NEW admission tests commit fixtures; other tests still roll back.
        for (long projectId : fixtureProjects) {
            var deviceIds = jdbc.queryForList("""
                SELECT d.id FROM device_asset d JOIN site s ON s.id=d.site_id
                WHERE s.project_id=?
                """, Long.class, projectId);
            for (long deviceId : deviceIds) {
                jdbc.update("DELETE FROM edge_diagnostic_reply WHERE device_id=?", deviceId);
                jdbc.update("DELETE FROM edge_diagnostic_request WHERE device_id=?", deviceId);
                jdbc.update("DELETE FROM operation_task WHERE id IN " +
                    "(SELECT task_id FROM operation_task_target WHERE device_id=?)", deviceId);
                jdbc.update("""
                    DELETE FROM audit_event WHERE resource_type='DEVICE'
                    AND resource_id=(SELECT machine_code FROM device_asset WHERE id=?)
                    """, deviceId);
                jdbc.update("DELETE FROM edge_realtime_lease WHERE device_id=?", deviceId);
                jdbc.update("DELETE FROM edge_system_monitor_lease WHERE device_id=?", deviceId);
                jdbc.update("DELETE FROM edge_realtime_point WHERE device_id=?", deviceId);
                jdbc.update("DELETE FROM device_asset WHERE id=?", deviceId);
            }
            jdbc.update("DELETE FROM site WHERE project_id=?", projectId);
            jdbc.update("DELETE FROM project WHERE id=?", projectId);
            assertEquals(0, jdbc.queryForObject("SELECT COUNT(*) FROM project WHERE id=?", Integer.class, projectId));
        }
        for (String id : jdbc.queryForList("SELECT PRIMARY_ID FROM SPRING_SESSION", String.class)) {
            if (!sessionsBefore.contains(id)) {
                jdbc.update("DELETE FROM SPRING_SESSION WHERE PRIMARY_ID=?", id);
            }
        }
    }

    @Test
    @WithUserDetails("admin")
    void mqttRealtimeSnapshotIsAvailableWithoutTheLegacyPlatform() throws Exception {
        ingress.ingest("edge/telemetry/realtime/" + machineCode, """
            {"type":"snapshot","machineCode":"%s","meters":[
              {"meterCode":"PCS_1","values":[
                {"index":110001,"pointCode":"active_power","name":"有功功率",
                 "value":12.3,"quality":0,"ts":1780000000000,"stale":false},
                {"index":110002,"pointCode":"soc","value":88,"quality":1,
                 "ts":1780000000001,"stale":true}
              ]}
            ]}
            """.formatted(machineCode));

        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/points", machineCode))
            .andExpect(status().isOk())
            .andExpect(header().string("X-Realtime-Scope", "unscoped"))
            .andExpect(jsonPath("$.data.length()").value(2))
            .andExpect(jsonPath("$.data[0].meterCode").value("PCS_1"))
            .andExpect(jsonPath("$.data[0].index").value(110001))
            .andExpect(jsonPath("$.data[0].pointCode").value("active_power"))
            .andExpect(jsonPath("$.data[0].value").value(12.3))
            .andExpect(jsonPath("$.data[0].quality").value(0))
            .andExpect(jsonPath("$.data[1].stale").value(true));
    }

    @Test
    @WithUserDetails("admin")
    void mqttRealtimeCompactArrayMatchesTheDeployedEdgeWireFormat() throws Exception {
        ingress.ingest("edge/telemetry/realtime/" + machineCode, """
            {"type":"telemetry","machineCode":"%s","meters":[
              {"meterCode":"T2216_LOCAL_DIO","values":[
                [210001,"DI_01",1,0,1780000000100,1780000005100,false],
                [210002,"DI_02",0,1,1780000000101,1780000005101,true]
              ]}
            ]}
            """.formatted(machineCode));

        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/points", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.length()").value(2))
            .andExpect(jsonPath("$.data[0].meterCode").value("T2216_LOCAL_DIO"))
            .andExpect(jsonPath("$.data[0].index").value(210001))
            .andExpect(jsonPath("$.data[0].pointCode").value("DI_01"))
            .andExpect(jsonPath("$.data[0].value").value(1))
            .andExpect(jsonPath("$.data[0].quality").value(0))
            .andExpect(jsonPath("$.data[1].stale").value(true));
    }

    @Test
    @WithUserDetails("admin")
    void fullSnapshotPersistsFourThousandCompactPoints() throws Exception {
        var root = objectMapper.createObjectNode();
        root.put("type", "telemetry");
        root.put("machineCode", machineCode);
        var values = root.putArray("meters").addObject()
            .put("meterCode", "BMS_1").putArray("values");
        for (int index = 1; index <= 4_000; index++) {
            values.addArray().add(600_000 + index).add("BMS_POINT_" + index)
                .add(index / 10.0).add(0).add(1_780_100_000_000L + index)
                .add(1_780_100_005_000L + index).add(false);
        }

        ingress.ingest("edge/telemetry/full/" + machineCode, root.toString());

        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/points", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.length()").value(4_000))
            .andExpect(jsonPath("$.data[0].index").value(600_001))
            .andExpect(jsonPath("$.data[3999].index").value(604_000));
    }

    @Test
    @WithUserDetails("admin")
    void olderRealtimeSnapshotCannotOverwriteANewerPoint() throws Exception {
        ingress.ingest("edge/telemetry/full/" + machineCode, """
            {"machineCode":"%s","meters":[{"meterCode":"PCS_1","values":[
              [110001,"active_power",30.5,0,1780200000200,1780200005200,false]
            ]}]}
            """.formatted(machineCode));
        ingress.ingest("edge/telemetry/full/" + machineCode, """
            {"machineCode":"%s","meters":[{"meterCode":"PCS_1","values":[
              [110001,"active_power",12.0,1,1780200000100,1780200005100,true]
            ]}]}
            """.formatted(machineCode));

        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/points", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.length()").value(1))
            .andExpect(jsonPath("$.data[0].value").value(30.5))
            .andExpect(jsonPath("$.data[0].quality").value(0))
            .andExpect(jsonPath("$.data[0].ts").value(1780200000200L))
            .andExpect(jsonPath("$.data[0].stale").value(false));
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    @WithUserDetails("admin")
    void fullNoSampleKeepsFirstIdentityButCannotEraseLastKnownSample(boolean compact) throws Exception {
        var point = FullNoSampleIngestTest.sentinel(compact);
        ingress.ingest("edge/telemetry/full/" + machineCode, FullNoSampleIngestTest.frame(machineCode, point));
        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/points", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.length()").value(1))
            .andExpect(jsonPath("$.data[0].index").value(4561))
            .andExpect(jsonPath("$.data[0].value").value(org.hamcrest.Matchers.nullValue()))
            .andExpect(jsonPath("$.data[0].quality").value(0))
            .andExpect(jsonPath("$.data[0].ts").value(0))
            .andExpect(jsonPath("$.data[0].stale").value(true));
        FullNoSampleIngestTest.set(point, "value", FullNoSampleIngestTest.JSON.getNodeFactory().numberNode(42));
        FullNoSampleIngestTest.set(point, "ts", FullNoSampleIngestTest.JSON.getNodeFactory().numberNode(1780000000000L));
        FullNoSampleIngestTest.set(point, "stale", FullNoSampleIngestTest.JSON.getNodeFactory().booleanNode(false));
        ingress.ingest("edge/telemetry/full/" + machineCode, FullNoSampleIngestTest.frame(machineCode, point));
        ingress.ingest("edge/telemetry/full/" + machineCode,
            FullNoSampleIngestTest.frame(machineCode, FullNoSampleIngestTest.sentinel(compact)));
        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/points", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.length()").value(1))
            .andExpect(jsonPath("$.data[0].value").value(42))
            .andExpect(jsonPath("$.data[0].ts").value(1780000000000L))
            .andExpect(jsonPath("$.data[0].stale").value(false));
    }

    @Test
    @WithUserDetails("admin")
    void nativeStatusEventMarksTheRegisteredDeviceOnlineAndStaleDevicesCanBecomeOffline() throws Exception {
        ingress.ingest("edge/status/" + machineCode, """
            {"service":"mqtt-driver","event":"started","ts":4102444800000}
            """);

        mockMvc.perform(get("/api/v2/devices/{machineCode}", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.connectionStatus").value("ONLINE"))
            .andExpect(jsonPath("$.data.lastSeenAt").isNotEmpty());

        edgeRuntimeRepository.markOfflineBefore(Instant.now().plusSeconds(1));
        mockMvc.perform(get("/api/v2/devices/{machineCode}", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.connectionStatus").value("OFFLINE"));
    }

    @Test
    @WithUserDetails("admin")
    void nativeConfigSnapshotReplyReplacesTheLegacySnapshotEndpoint() throws Exception {
        ingress.ingest("edge/config/pull/reply/" + machineCode, """
            {"requestId":"CFG-SNAPSHOT-001","machineCode":"%s","success":true,
             "fileCount":2,"skippedFiles":0,"totalBytes":128,
             "files":[{"path":"runtime/apps/mqtt-service.json","sizeBytes":64},
                      {"path":"runtime/device_identity.json","sizeBytes":64}],
             "ts":1780000001000}
            """.formatted(machineCode));

        mockMvc.perform(get("/api/v2/devices/{machineCode}/system-monitor/snapshot", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.source").value("NATIVE_MQTT"))
            .andExpect(jsonPath("$.data.requestId").value("CFG-SNAPSHOT-001"))
            .andExpect(jsonPath("$.data.success").value(true))
            .andExpect(jsonPath("$.data.fileCount").value(2))
            .andExpect(jsonPath("$.data.files.length()").value(2));
    }

    @Test
    @WithUserDetails("admin")
    void chunkedConfigSnapshotIsAssembledBeforeItBecomesVisible() throws Exception {
        String snapshot = """
            {"requestId":"CFG-CHUNKED-001","machineCode":"%s","success":true,
             "fileCount":1,"files":[{"path":"runtime/device_identity.json","sizeBytes":32}],
             "ts":1780000001200}
            """.formatted(machineCode);
        int split = snapshot.length() / 2;
        for (int index : new int[]{2, 1}) {
            String part = index == 1 ? snapshot.substring(0, split) : snapshot.substring(split);
            var wrapper = objectMapper.createObjectNode();
            wrapper.put("chunked", true);
            wrapper.put("requestId", "CFG-CHUNKED-001");
            wrapper.put("machineCode", machineCode);
            wrapper.put("chunkIndex", index);
            wrapper.put("chunkCount", 2);
            wrapper.put("totalBytes", snapshot.getBytes(java.nio.charset.StandardCharsets.UTF_8).length);
            wrapper.put("payloadPart", part);
            ingress.ingest("edge/config/pull/reply/" + machineCode, wrapper.toString());
        }

        mockMvc.perform(get("/api/v2/devices/{machineCode}/system-monitor/snapshot", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.requestId").value("CFG-CHUNKED-001"))
            .andExpect(jsonPath("$.data.fileCount").value(1));
    }

    @Test
    @WithUserDetails("admin")
    void operatorCanRequestANewConfigSnapshotOverNativeMqtt() throws Exception {
        when(transport.ready()).thenReturn(true);

        mockMvc.perform(post("/api/v2/devices/{machineCode}/system-monitor/snapshot/requests", machineCode)
                .with(csrf()))
            .andExpect(status().isAccepted())
            .andExpect(jsonPath("$.data.requestId").isNotEmpty())
            .andExpect(jsonPath("$.data.status").value("REQUESTED"));

        verify(transport).publish(eq("edge/config/pull/request/" + machineCode), any(byte[].class), eq(2));
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void operatorCanRequestFreshSystemTelemetryOverNativeMqtt() throws Exception {
        when(transport.ready()).thenReturn(true);

        mockMvc.perform(post("/api/v2/devices/{machineCode}/system-monitor/telemetry/requests", machineCode)
                .with(csrf()))
            .andExpect(status().isAccepted())
            .andExpect(jsonPath("$.data.requestId",
                org.hamcrest.Matchers.startsWith("MONITOR_")))
            .andExpect(jsonPath("$.data.status").value("REQUESTED"))
            .andExpect(jsonPath("$.data.requestedAt").isNumber())
            .andExpect(jsonPath("$.data.ttlSec").value(15));

        verify(transport).publish(eq("edge/system/monitor/request/" + machineCode),
            argThat(bytes -> {
                String payload = new String(bytes, java.nio.charset.StandardCharsets.UTF_8);
                return payload.contains("\"action\":\"subscribe\"")
                    && payload.contains("\"sessionId\":\"MONITOR_")
                    && payload.contains("\"intervalMs\":1000")
                    && payload.contains("\"ttlSec\":15");
            }), eq(2));
    }

    @Test
    @WithUserDetails("admin")
    void monitorRejectsNullAndMalformedSessionBeforeMqtt() {
        assertThrows(IllegalArgumentException.class, () -> runtime.unsubscribeSystemTelemetry(machineCode, null));
        assertThrows(IllegalArgumentException.class, () -> runtime.unsubscribeSystemTelemetry(machineCode, ""));
        assertThrows(IllegalArgumentException.class, () -> runtime.unsubscribeSystemTelemetry(machineCode, "REALTIME_foreign"));
        verify(transport, never()).publish(anyString(), any(byte[].class), anyInt());
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void monitorPublishFailureRetainsOnlyUncertainLeaseForTtl(boolean uncertain) {
        when(transport.ready()).thenReturn(true);
        doThrow(new EdgeTransportException("local test failure", null, uncertain))
            .when(transport).publish(anyString(), any(byte[].class), anyInt());
        assertThrows(EdgeTransportException.class, () -> runtime.requestSystemTelemetry(machineCode));
        long device = edgeRuntimeRepository.findDeviceId(machineCode).orElseThrow();
        assertEquals(uncertain ? 1 : 0, jdbc.queryForObject(
            "SELECT COUNT(*) FROM edge_system_monitor_lease WHERE device_id=?", Integer.class, device));
        if (uncertain) {
            Instant expires = jdbc.queryForObject(
                "SELECT expires_at FROM edge_system_monitor_lease WHERE device_id=?", Timestamp.class, device).toInstant();
            assertTrue(expires.isAfter(Instant.now()));
            assertTrue(expires.isBefore(Instant.now().plusSeconds(16)));
        }
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void monitorStopFailureKeepsOwnerForRetry() {
        when(transport.ready()).thenReturn(true);
        String session = runtime.requestSystemTelemetry(machineCode).requestId();
        long device = edgeRuntimeRepository.findDeviceId(machineCode).orElseThrow();
        long owner = identities.findByUsername("admin").orElseThrow().id();
        doThrow(new EdgeTransportException("local stop failure", null, true)).doNothing()
            .when(transport).publish(anyString(), any(byte[].class), anyInt());
        assertThrows(EdgeTransportException.class, () -> runtime.unsubscribeSystemTelemetry(machineCode, session));
        assertTrue(edgeRuntimeRepository.systemMonitorLeaseOwnedBy(session, device, owner));
        runtime.unsubscribeSystemTelemetry(machineCode, session);
        assertFalse(edgeRuntimeRepository.systemMonitorLeaseOwnedBy(session, device, owner));
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void operatorCanStopSystemTelemetryLeaseOverNativeMqtt() throws Exception {
        when(transport.ready()).thenReturn(true);
        String sessionId = objectMapper.readTree(mockMvc.perform(post(
                "/api/v2/devices/{machineCode}/system-monitor/telemetry/requests", machineCode).with(csrf()))
            .andExpect(status().isAccepted()).andReturn().getResponse().getContentAsString())
            .path("data").path("requestId").asText();
        clearInvocations(transport);

        mockMvc.perform(delete(
                "/api/v2/devices/{machineCode}/system-monitor/telemetry/requests/{requestId}",
                machineCode, sessionId).with(csrf()))
            .andExpect(status().isAccepted())
            .andExpect(jsonPath("$.data.requestId").value(sessionId))
            .andExpect(jsonPath("$.data.action").value("UNSUBSCRIBE"))
            .andExpect(jsonPath("$.data.status").value("REQUESTED"));

        verify(transport).publish(eq("edge/system/monitor/request/" + machineCode),
            argThat(bytes -> {
                String payload = new String(bytes, java.nio.charset.StandardCharsets.UTF_8);
                return payload.contains("\"action\":\"unsubscribe\"")
                    && payload.contains("\"sessionId\":\"" + sessionId + "\"");
            }), eq(2));
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void operatorCanStartAndStopAReadOnlyRealtimeLease() throws Exception {
        when(transport.ready()).thenReturn(true);

        MvcResult subscribed = mockMvc.perform(post(
                "/api/v2/devices/{machineCode}/realtime/subscriptions", machineCode)
                .with(csrf()).contentType("application/json")
                .content("""
                    {"meterCode":"PCS_1","intervalMs":1000,"ttlSec":30}
                    """))
            .andExpect(status().isAccepted())
            .andExpect(jsonPath("$.data.sessionId").value(org.hamcrest.Matchers.startsWith("REALTIME_")))
            .andExpect(jsonPath("$.data.action").value("SUBSCRIBE"))
            .andReturn();
        String sessionId = objectMapper.readTree(subscribed.getResponse().getContentAsString())
            .path("data").path("sessionId").asText();

        verify(transport).publish(eq("edge/telemetry/realtime/request/" + machineCode),
            argThat(bytes -> new String(bytes, java.nio.charset.StandardCharsets.UTF_8)
                .contains("\"action\":\"subscribe\"")), eq(2));

        mockMvc.perform(delete(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}",
                machineCode, sessionId).with(csrf()))
            .andExpect(status().isAccepted())
            .andExpect(jsonPath("$.data.sessionId").value(sessionId))
            .andExpect(jsonPath("$.data.action").value("UNSUBSCRIBE"));

        verify(transport).publish(eq("edge/telemetry/realtime/request/" + machineCode),
            argThat(bytes -> new String(bytes, java.nio.charset.StandardCharsets.UTF_8)
                .contains("\"action\":\"unsubscribe\"")), eq(2));
        verify(transport, never()).publish(argThat(topic ->
            topic != null && topic.startsWith("edge/telemetry/full/")), any(byte[].class), anyInt());

        ingress.ingest("edge/telemetry/full/" + machineCode, """
            {"machineCode":"%s","meters":[{"meterCode":"PCS_1","values":[
              [120001,"active_power",42.5,0,1780300000200,1780300005200,false]
            ]}]}
            """.formatted(machineCode));

        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/points", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.length()").value(1))
            .andExpect(jsonPath("$.data[0].meterCode").value("PCS_1"))
            .andExpect(jsonPath("$.data[0].index").value(120001))
            .andExpect(jsonPath("$.data[0].value").value(42.5));
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void sessionRealtimeSeparatesMeterSelectorsAndRejectsLateOrForeignPayloads() throws Exception {
        when(transport.ready()).thenReturn(true);
        String sessionA = subscribeRealtime("METER_A");
        String sessionB = subscribeRealtime("METER_B");

        ingress.ingest("edge/telemetry/realtime/" + machineCode, """
            {"machineCode":"%s","sessionId":"%s","meters":[
              {"meterCode":"METER_A","values":[
                [110001,"A_POWER",11.1,0,1780400000100,1780400005100,false]
              ]}
            ]}
            """.formatted(machineCode, sessionA));
        ingress.ingest("edge/telemetry/realtime/" + machineCode, """
            {"machineCode":"%s","sessionId":"%s","meters":[
              {"meterCode":"METER_B","values":[
                [220001,"B_POWER",22.2,0,1780400000200,1780400005200,false]
              ]}
            ]}
            """.formatted(machineCode, sessionB));

        mockMvc.perform(get(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}/points",
                machineCode, sessionA))
            .andExpect(status().isOk())
            .andExpect(header().string("X-Realtime-Scope", "session"))
            .andExpect(jsonPath("$.data.length()").value(1))
            .andExpect(jsonPath("$.data[0].meterCode").value("METER_A"))
            .andExpect(jsonPath("$.data[0].value").value(11.1));
        mockMvc.perform(get(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}/points",
                machineCode, sessionB))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.length()").value(1))
            .andExpect(jsonPath("$.data[0].meterCode").value("METER_B"))
            .andExpect(jsonPath("$.data[0].value").value(22.2));
        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/points", machineCode))
            .andExpect(status().isOk())
            .andExpect(header().string("X-Realtime-Scope", "unscoped"))
            .andExpect(jsonPath("$.data.length()").value(2))
            .andExpect(jsonPath("$.data[0].meterCode").value("METER_A"))
            .andExpect(jsonPath("$.data[0].value").value(11.1))
            .andExpect(jsonPath("$.data[1].meterCode").value("METER_B"))
            .andExpect(jsonPath("$.data[1].value").value(22.2));

        assertThrows(IllegalArgumentException.class, () -> ingress.ingest(
            "edge/telemetry/realtime/" + machineCode, """
                {"machineCode":"%s","sessionId":"%s","meters":[
                  {"meterCode":"METER_B","values":[
                    [110002,"WRONG_SELECTOR",99,0,1780400000300,1780400005300,false]
                  ]}
                ]}
                """.formatted(machineCode, sessionA)));
        assertThrows(IllegalArgumentException.class, () -> ingress.ingest(
            "edge/telemetry/realtime/" + machineCode, """
                {"machineCode":"%s","sessionId":"REALTIME_unknown","meters":[
                  {"meterCode":"METER_X","values":[
                    [990001,"UNKNOWN_SESSION",99,0,1780400000350,1780400005350,false]
                  ]}
                ]}
                """.formatted(machineCode)));
        assertThrows(IllegalArgumentException.class, () -> ingress.ingest(
            "edge/telemetry/realtime/" + machineCode, """
                {"machineCode":"%s","sessionId":42,"meters":[]}
                """.formatted(machineCode)));
        String expiredSession = "REALTIME_" + UUID.randomUUID();
        long currentDeviceId = edgeRuntimeRepository.findDeviceId(machineCode).orElseThrow();
        edgeRuntimeRepository.createRealtimeLease(expiredSession, currentDeviceId,
            "METER_A", 1000, Instant.now().minusSeconds(1), identities.findByUsername("admin").orElseThrow().id());
        assertTrue(edgeRuntimeRepository.markRealtimeLeaseActive(
            expiredSession, currentDeviceId) == 0);
        assertThrows(IllegalArgumentException.class, () -> ingress.ingest(
            "edge/telemetry/realtime/" + machineCode, """
                {"machineCode":"%s","sessionId":"%s","meters":[
                  {"meterCode":"METER_A","values":[]}
                ]}
                """.formatted(machineCode, expiredSession)));
        mockMvc.perform(get(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}/points",
                machineCode, expiredSession))
            .andExpect(status().isBadRequest());

        long adminId = identities.findByUsername("admin").orElseThrow().id();
        long projectId = assets.createProject("EDGE_FOREIGN_" + UUID.randomUUID(),
            "会话设备归属测试项目", adminId);
        fixtureProjects.add(projectId);
        long siteId = assets.createSite(projectId, "EDGE_FOREIGN_SITE_" + UUID.randomUUID(),
            "会话设备归属测试站点", "Asia/Shanghai", adminId);
        String otherMachineCode = "GW-NATIVE-FOREIGN-" + UUID.randomUUID().toString().substring(0, 8);
        assets.createDevice(siteId, otherMachineCode, "会话设备归属测试设备", "EDGE-GW", "MANUAL", adminId);
        assertThrows(IllegalArgumentException.class, () -> ingress.ingest(
            "edge/telemetry/realtime/" + otherMachineCode, """
                {"machineCode":"%s","sessionId":"%s","meters":[
                  {"meterCode":"METER_A","values":[
                    [110003,"FOREIGN_DEVICE",88,0,1780400000375,1780400005375,false]
                  ]}
                ]}
                """.formatted(otherMachineCode, sessionA)));
        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/points", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.length()").value(2))
            .andExpect(jsonPath("$.data[0].value").value(11.1))
            .andExpect(jsonPath("$.data[1].value").value(22.2));

        mockMvc.perform(delete(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}",
                machineCode, sessionA).with(csrf()))
            .andExpect(status().isAccepted());
        assertThrows(IllegalArgumentException.class, () -> ingress.ingest(
            "edge/telemetry/realtime/" + machineCode, """
                {"machineCode":"%s","sessionId":"%s","meters":[
                  {"meterCode":"METER_A","values":[
                    [110001,"A_POWER",77.7,0,1780400000400,1780400005400,false]
                  ]}
                ]}
                """.formatted(machineCode, sessionA)));
        mockMvc.perform(get(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}/points",
                machineCode, sessionA))
            .andExpect(status().isBadRequest());
        mockMvc.perform(get(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}/points",
                machineCode, sessionB))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data[0].value").value(22.2));
        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/points", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.length()").value(2))
            .andExpect(jsonPath("$.data[0].value").value(11.1));

        ingress.ingest("edge/telemetry/full/" + machineCode, """
            {"machineCode":"%s","meters":[{"meterCode":"FULL_METER","values":[
              [330001,"FULL_POWER",33.3,0,1780400000500,1780400005500,false]
            ]}]}
            """.formatted(machineCode));
        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/points", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.length()").value(3))
            .andExpect(jsonPath("$.data[0].meterCode").value("FULL_METER"))
            .andExpect(jsonPath("$.data[1].meterCode").value("METER_A"))
            .andExpect(jsonPath("$.data[2].meterCode").value("METER_B"));
        mockMvc.perform(get(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}/points",
                machineCode, sessionB))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.length()").value(1))
            .andExpect(jsonPath("$.data[0].meterCode").value("METER_B"));
    }

    private String feedback(String sessionId, String event, String reason) {
        return """
            {"service":"mqtt-driver","machineCode":"%s","event":"%s","sessionId":"%s",
             "ts":1780000000000,"reasonCode":"%s"}
            """.formatted(machineCode, event, sessionId, reason);
    }

    private void assertLeaseStatus(String sessionId, String expected) throws Exception {
        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}", machineCode, sessionId))
            .andExpect(status().isOk()).andExpect(jsonPath("$.data.status").value(expected));
    }

    @ParameterizedTest
    @ValueSource(strings = {"realtime-session-started", "realtime-session-rejected"})
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void admissionCallbackDuringPublishIsNotOverwritten(String event) throws Exception {
        when(transport.ready()).thenReturn(true);
        doAnswer(invocation -> {
            String session = objectMapper.readTree((byte[]) invocation.getArgument(1)).path("sessionId").asText();
            ingress.ingest("edge/status/" + machineCode, feedback(session, event, "CAPACITY_REACHED"));
            return null;
        }).when(transport).publish(anyString(), any(byte[].class), anyInt());
        String session = subscribeRealtime("PCS_1");
        String expected = event.endsWith("started") ? "ACTIVE" : "REJECTED";
        assertLeaseStatus(session, expected);
        ingress.ingest("edge/status/" + machineCode, feedback(session,
            event.endsWith("started") ? "realtime-session-rejected" : "realtime-session-started", "INVALID_REQUEST"));
        assertLeaseStatus(session, expected);
        if (expected.equals("REJECTED")) {
            mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}", machineCode, session))
                .andExpect(jsonPath("$.data.reasonCode").value("CAPACITY_REACHED"));
            clearInvocations(transport);
            mockMvc.perform(delete("/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}", machineCode, session).with(csrf()))
                .andExpect(status().isAccepted());
            assertTrue(edgeRuntimeRepository.realtimeLease(session).isEmpty());
            verify(transport, never()).publish(anyString(), any(byte[].class), anyInt());
        }
    }

    @ParameterizedTest
    @ValueSource(strings = {"STOPPING", "STOP_FAILED", "REJECTED", "EXPIRED", "DELETED"})
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void lateAdmissionCannotReviveTerminalOrExpiredSession(String state) throws Exception {
        when(transport.ready()).thenReturn(true);
        String session = subscribeRealtime("PCS_1");
        if (state.equals("DELETED")) {
            mockMvc.perform(delete("/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}", machineCode, session).with(csrf()))
                .andExpect(status().isAccepted());
        } else if (state.equals("EXPIRED")) {
            jdbc.update("UPDATE edge_realtime_lease SET expires_at=? WHERE session_id=?", Timestamp.from(Instant.now().minusSeconds(1)), session);
        } else jdbc.update("UPDATE edge_realtime_lease SET lease_state=? WHERE session_id=?", state, session);
        for (String event : List.of("realtime-session-started", "realtime-session-rejected")) {
            ingress.ingest("edge/status/" + machineCode, feedback(session, event, "CAPACITY_REACHED"));
        }
        assertThrows(EdgeMessageRejectedException.class, () -> ingress.ingest("edge/telemetry/realtime/" + machineCode,
            scopedSample(session, "PCS_1", "[1,\"P\",2,0,1780000000000]")));
        if (state.equals("DELETED")) assertTrue(edgeRuntimeRepository.realtimeLease(session).isEmpty());
        else assertLeaseStatus(session, state);
    }

    private String scopedSample(String session, String meter, String value) {
        return """
            {"machineCode":"%s","sessionId":"%s","meters":[{"meterCode":"%s","values":[%s]}]}
            """.formatted(machineCode, session, meter, value);
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void onlyFullyValidatedScopedDataPromotesPendingAndFullContinues() throws Exception {
        when(transport.ready()).thenReturn(true);
        String session = subscribeRealtime("PCS_1");
        String valid = "[1,\"P\",2,0,1780000000000]";
        for (String invalid : List.of("[2]", "[\"2\",\"P\",2,0,1780000000000]", "{}", "null",
            "[2,\"P\",2,0,1780000000000,1780000005000,\"false\"]", "[2,{},2,0,1780000000000]")) {
            assertThrows(EdgeMessageRejectedException.class, () -> ingress.ingest("edge/telemetry/realtime/" + machineCode,
                scopedSample(session, "PCS_1", valid + "," + invalid)));
            assertLeaseStatus(session, "REQUESTED");
        }
        assertThrows(EdgeMessageRejectedException.class, () -> ingress.ingest("edge/telemetry/realtime/" + machineCode,
            scopedSample(session, "OTHER", valid)));
        ingress.ingest("edge/telemetry/full/" + machineCode, scopedSample(session, "PCS_1", valid));
        assertLeaseStatus(session, "REQUESTED");
        ingress.ingest("edge/telemetry/realtime/" + machineCode,
            scopedSample(session, "PCS_1", valid).replace("\"sessionId\":\"" + session + "\",", ""));
        assertLeaseStatus(session, "REQUESTED");
        ingress.ingest("edge/telemetry/realtime/" + machineCode, scopedSample(session, "PCS_1", valid));
        assertLeaseStatus(session, "ACTIVE");
        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}/points", machineCode, session))
            .andExpect(status().isOk()).andExpect(jsonPath("$.data.length()").value(1));
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void feedbackValidatesIdentityAndStatusEndpointAuthorization() throws Exception {
        when(transport.ready()).thenReturn(true);
        String session = subscribeRealtime("PCS_1");
        String valid = feedback(session, "realtime-session-started", "");
        for (String invalid : List.of(valid.replace("mqtt-driver", "other"), valid.replace(machineCode, "OTHER"),
            valid.replace("1780000000000", "\"bad\""), valid.replace(session, "bad id"),
            valid.replace("\"event\":", "\"sessionId\":\"REALTIME_other\",\"event\":"))) {
            assertThrows(EdgeMessageRejectedException.class, () -> ingress.ingest("edge/status/" + machineCode, invalid));
        }
        assertThrows(EdgeMessageRejectedException.class, () -> ingress.ingest("edge/status/nested/" + machineCode, valid));
        assertThrows(EdgeMessageRejectedException.class, () -> ingress.ingest("edge/status/" + machineCode,
            feedback(session, "realtime-session-rejected", "RAW_EXCEPTION")));
        long deviceId = edgeRuntimeRepository.findDeviceId(machineCode).orElseThrow();
        long siteId = jdbc.queryForObject("SELECT site_id FROM device_asset WHERE id=?", Long.class, deviceId);
        assets.createDevice(siteId, "OTHER_" + machineCode, "Other", "EDGE-GW", "MANUAL", identities.findByUsername("admin").orElseThrow().id());
        ingress.ingest("edge/status/OTHER_" + machineCode, valid.replace(machineCode, "OTHER_" + machineCode));
        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}", "OTHER_" + machineCode, session))
            .andExpect(status().isBadRequest());
        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}", machineCode, session)
                .with(user("no-realtime").authorities(new org.springframework.security.core.authority.SimpleGrantedAuthority("device:read"))))
            .andExpect(status().isForbidden());
        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}", machineCode, session)
                .with(org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.anonymous()))
            .andExpect(status().isUnauthorized());
        assertLeaseStatus(session, "REQUESTED");
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void retainedRejectedLeasesDoNotConsumeAdmissionBudget() throws Exception {
        when(transport.ready()).thenReturn(true);
        for (int index = 0; index < 9; index++) {
            String session = subscribeRealtime("PCS_1");
            ingress.ingest("edge/status/" + machineCode, feedback(session, "realtime-session-rejected", "INVALID_REQUEST"));
        }
        assertLeaseStatus(subscribeRealtime("PCS_1"), "REQUESTED");
    }

    private String subscribeRealtime(String meterCode) throws Exception {
        MvcResult result = mockMvc.perform(post(
                "/api/v2/devices/{machineCode}/realtime/subscriptions", machineCode)
                .with(csrf()).contentType("application/json")
                .content("""
                    {"meterCode":"%s","intervalMs":1000,"ttlSec":30}
                    """.formatted(meterCode)))
            .andExpect(status().isAccepted())
            .andReturn();
        return objectMapper.readTree(result.getResponse().getContentAsString())
            .path("data").path("sessionId").asText();
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void ninthUnexpiredRealtimeLeaseIsRejectedBeforePublishWithoutEvictingEightSessions() throws Exception {
        when(transport.ready()).thenReturn(true);
        doAnswer(invocation -> {
            assertFalse(TransactionSynchronizationManager.isActualTransactionActive());
            String sessionId = objectMapper.readTree((byte[]) invocation.getArgument(1))
                .path("sessionId").asText();
            assertEquals("STARTING", edgeRuntimeRepository.realtimeLease(sessionId).orElseThrow().state());
            return null;
        }).when(transport).publish(anyString(), any(byte[].class), anyInt());
        var sessions = new ArrayList<String>();
        for (int index = 0; index < 8; index++) {
            sessions.add(subscribeRealtime("METER_" + index));
        }
        clearInvocations(transport);

        mockMvc.perform(post("/api/v2/devices/{machineCode}/realtime/subscriptions", machineCode)
                .with(csrf()).contentType("application/json")
                .content("""
                    {"meterCode":"METER_REPLACEMENT","intervalMs":1000,"ttlSec":30}
                    """))
            .andExpect(status().isTooManyRequests())
            .andExpect(jsonPath("$.error.code").value("REALTIME_LEASE_LIMIT_EXCEEDED"));

        verify(transport, never()).publish(anyString(), any(byte[].class), anyInt());
        long deviceId = edgeRuntimeRepository.findDeviceId(machineCode).orElseThrow();
        assertEquals(8L, edgeRuntimeRepository.realtimeLeaseCount(deviceId));
        for (String session : sessions) {
            assertTrue(edgeRuntimeRepository.realtimeLease(session).isPresent());
        }
        ingress.ingest("edge/telemetry/full/" + machineCode, """
            {"machineCode":"%s","meters":[{"meterCode":"FULL_METER","values":[
              [330001,"FULL_POWER",33.3,0,1780400000500,1780400005500,false]
            ]}]}
            """.formatted(machineCode));
        mockMvc.perform(get("/api/v2/devices/{machineCode}/realtime/points", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data[0].meterCode").value("FULL_METER"))
            .andExpect(jsonPath("$.data[0].value").value(33.3));
        assertEquals(8L, edgeRuntimeRepository.realtimeLeaseCount(deviceId));
        verify(transport, never()).publish(anyString(), any(byte[].class), anyInt());
    }

    @ParameterizedTest
    @ValueSource(strings = {"STARTING", "ACTIVE", "STOPPING", "STOP_FAILED"})
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void everyUnexpiredStateCountsButExpiredHistoryDoesNotConsumeAdmission(String state) throws Exception {
        when(transport.ready()).thenReturn(true);
        String oldest = subscribeRealtime("METER_0");
        for (int index = 1; index < 8; index++) subscribeRealtime("METER_" + index);
        long deviceId = edgeRuntimeRepository.findDeviceId(machineCode).orElseThrow();
        jdbc.update("UPDATE edge_realtime_lease SET lease_state=? WHERE device_id=?", state, deviceId);
        clearInvocations(transport);

        mockMvc.perform(post("/api/v2/devices/{machineCode}/realtime/subscriptions", machineCode)
                .with(csrf()).contentType("application/json")
                .content("""
                    {"meterCode":"METER_NEXT","intervalMs":1000,"ttlSec":30}
                    """))
            .andExpect(status().isTooManyRequests());
        verify(transport, never()).publish(anyString(), any(byte[].class), anyInt());

        jdbc.update("UPDATE edge_realtime_lease SET expires_at=? WHERE session_id=?",
            Timestamp.from(Instant.now().minusSeconds(1)), oldest);
        subscribeRealtime("METER_NEXT");
        new EdgeRuntimeService(devices, edgeRuntimeRepository, objectMapper, operations)
            .cleanupExpiredRealtimeLeases();
        assertEquals(9L, edgeRuntimeRepository.realtimeLeaseCount(deviceId));
        assertEquals(state, edgeRuntimeRepository.realtimeLease(oldest).orElseThrow().state());
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void fullDeviceDoesNotConsumeAnotherDevicesAdmission() throws Exception {
        when(transport.ready()).thenReturn(true);
        for (int index = 0; index < 8; index++) subscribeRealtime("METER_" + index);
        long fullDeviceId = edgeRuntimeRepository.findDeviceId(machineCode).orElseThrow();
        long siteId = jdbc.queryForObject("SELECT site_id FROM device_asset WHERE id=?", Long.class, fullDeviceId);
        long adminId = identities.findByUsername("admin").orElseThrow().id();
        String otherMachine = "GW-ADMISSION-" + UUID.randomUUID().toString().substring(0, 8);
        long otherDeviceId = assets.createDevice(siteId, otherMachine, "Admission fixture", "EDGE-GW", "MANUAL", adminId);

        mockMvc.perform(post("/api/v2/devices/{machineCode}/realtime/subscriptions", otherMachine)
                .with(csrf()).contentType("application/json")
                .content("""
                    {"meterCode":"METER_0","intervalMs":1000,"ttlSec":30}
                    """))
            .andExpect(status().isAccepted());
        assertEquals(8L, edgeRuntimeRepository.realtimeLeaseCount(fullDeviceId));
        assertEquals(1L, edgeRuntimeRepository.realtimeLeaseCount(otherDeviceId));
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void twoRepositoriesSerializeTheLastSlotAndRereadAfterCommit() throws Exception {
        Authentication authentication = SecurityContextHolder.getContext().getAuthentication();
        when(transport.ready()).thenReturn(true);
        for (int index = 0; index < 7; index++) subscribeRealtime("METER_" + index);
        clearInvocations(transport);
        var firstJdbc = spy(new NamedParameterJdbcTemplate(jdbc.getDataSource()));
        var secondJdbc = spy(new NamedParameterJdbcTemplate(jdbc.getDataSource()));
        var beforeInsert = new CountDownLatch(1);
        var allowInsert = new CountDownLatch(1);
        var secondLockAttempt = new CountDownLatch(1);
        var counts = new java.util.concurrent.CopyOnWriteArrayList<Long>();
        doAnswer(invocation -> {
            long count = (Long) invocation.callRealMethod();
            counts.add(count);
            beforeInsert.countDown();
            assertTrue(allowInsert.await(5, TimeUnit.SECONDS));
            return count;
        }).when(firstJdbc).queryForObject(argThat(sql -> sql != null && sql.contains("COUNT(*)")),
            any(MapSqlParameterSource.class), eq(Long.class));
        doAnswer(invocation -> {
            secondLockAttempt.countDown();
            return invocation.callRealMethod();
        }).when(secondJdbc).queryForObject(argThat(sql -> sql != null && sql.contains("FOR UPDATE")),
            any(MapSqlParameterSource.class), eq(Long.class));
        doAnswer(invocation -> {
            long count = (Long) invocation.callRealMethod();
            counts.add(count);
            return count;
        }).when(secondJdbc).queryForObject(argThat(sql -> sql != null && sql.contains("COUNT(*)")),
            any(MapSqlParameterSource.class), eq(Long.class));
        var first = new EdgeRuntimeService(devices, admissionRepository(firstJdbc), objectMapper, operations);
        var second = new EdgeRuntimeService(devices, admissionRepository(secondJdbc), objectMapper, operations);
        var executor = Executors.newFixedThreadPool(2);
        try {
            var admitted = executor.submit(() -> subscribeAs(first, authentication));
            assertTrue(beforeInsert.await(5, TimeUnit.SECONDS));
            var rejected = executor.submit(() -> assertThrows(RealtimeLeaseLimitExceededException.class,
                () -> subscribeAs(second, authentication)));
            assertTrue(secondLockAttempt.await(5, TimeUnit.SECONDS));
            assertThrows(TimeoutException.class, () -> rejected.get(200, TimeUnit.MILLISECONDS));
            allowInsert.countDown();
            assertTrue(admitted.get(5, TimeUnit.SECONDS).startsWith("REALTIME_"));
            rejected.get(5, TimeUnit.SECONDS);
            assertEquals(List.of(7L, 8L), counts);
            assertEquals(8L, edgeRuntimeRepository.realtimeLeaseCount(
                edgeRuntimeRepository.findDeviceId(machineCode).orElseThrow()));
            verify(transport, times(1)).publish(anyString(), any(byte[].class), anyInt());
        } finally {
            allowInsert.countDown();
            executor.shutdownNow();
            assertTrue(executor.awaitTermination(5, TimeUnit.SECONDS));
        }
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void mqttWaitDoesNotHoldAdmissionTransactionOrDeviceLock() throws Exception {
        Authentication authentication = SecurityContextHolder.getContext().getAuthentication();
        when(transport.ready()).thenReturn(true);
        for (int index = 0; index < 7; index++) subscribeRealtime("METER_" + index);
        clearInvocations(transport);
        var publishing = new CountDownLatch(1);
        var allowPublish = new CountDownLatch(1);
        doAnswer(invocation -> {
            assertFalse(TransactionSynchronizationManager.isActualTransactionActive());
            publishing.countDown();
            assertTrue(allowPublish.await(5, TimeUnit.SECONDS));
            return null;
        }).when(transport).publish(anyString(), any(byte[].class), anyInt());
        var first = new EdgeRuntimeService(devices,
            admissionRepository(new NamedParameterJdbcTemplate(jdbc.getDataSource())), objectMapper, operations);
        var second = new EdgeRuntimeService(devices,
            admissionRepository(new NamedParameterJdbcTemplate(jdbc.getDataSource())), objectMapper, operations);
        var executor = Executors.newFixedThreadPool(2);
        try {
            var admitted = executor.submit(() -> subscribeAs(first, authentication));
            assertTrue(publishing.await(5, TimeUnit.SECONDS));
            var rejected = executor.submit(() -> assertThrows(RealtimeLeaseLimitExceededException.class,
                () -> subscribeAs(second, authentication)));
            rejected.get(3, TimeUnit.SECONDS);
            assertFalse(admitted.isDone());
            allowPublish.countDown();
            admitted.get(5, TimeUnit.SECONDS);
            verify(transport, times(1)).publish(anyString(), any(byte[].class), anyInt());
        } finally {
            allowPublish.countDown();
            executor.shutdownNow();
            assertTrue(executor.awaitTermination(5, TimeUnit.SECONDS));
        }
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void admissionCommitsIndependentlyOfAnOuterRepeatableReadTransaction() {
        long deviceId = edgeRuntimeRepository.findDeviceId(machineCode).orElseThrow();
        var observedJdbc = spy(new NamedParameterJdbcTemplate(jdbc.getDataSource()));
        doAnswer(invocation -> {
            assertEquals(Connection.TRANSACTION_READ_COMMITTED,
                TransactionSynchronizationManager.getCurrentTransactionIsolationLevel());
            return invocation.callRealMethod();
        }).when(observedJdbc).queryForObject(anyString(), any(MapSqlParameterSource.class), eq(Long.class));
        var repository = admissionRepository(observedJdbc);
        var outer = new TransactionTemplate(new DataSourceTransactionManager(jdbc.getDataSource()));
        outer.setIsolationLevel(Connection.TRANSACTION_REPEATABLE_READ);
        String session = "REALTIME_" + UUID.randomUUID();
        outer.executeWithoutResult(status -> {
            assertEquals(0L, edgeRuntimeRepository.realtimeLeaseCount(deviceId));
            repository.createRealtimeLease(session, deviceId, null, 1000, Instant.now().plusSeconds(30),
                identities.findByUsername("admin").orElseThrow().id());
            assertEquals(Connection.TRANSACTION_REPEATABLE_READ,
                TransactionSynchronizationManager.getCurrentTransactionIsolationLevel());
            status.setRollbackOnly();
        });
        assertTrue(edgeRuntimeRepository.realtimeLease(session).isPresent());
        assertFalse(TransactionSynchronizationManager.isActualTransactionActive());
    }

    private EdgeRuntimeRepository admissionRepository(NamedParameterJdbcTemplate template) {
        var interceptor = new TransactionInterceptor();
        interceptor.setTransactionManager(new DataSourceTransactionManager(template.getJdbcTemplate().getDataSource()));
        interceptor.setTransactionAttributeSource(new AnnotationTransactionAttributeSource());
        var factory = new ProxyFactory(new EdgeRuntimeRepository(template, objectMapper));
        factory.setProxyTargetClass(true);
        factory.addAdvice(interceptor);
        return (EdgeRuntimeRepository) factory.getProxy();
    }

    private String subscribeAs(EdgeRuntimeService service, Authentication authentication) {
        SecurityContextHolder.getContext().setAuthentication(authentication);
        try {
            return service.subscribeRealtime(machineCode, "METER_NEXT", 1000, 30);
        } finally {
            SecurityContextHolder.clearContext();
        }
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void realtimeDeleteRejectsMonitorUnknownAndCrossDeviceSessionsBeforeMqttPublish() throws Exception {
        when(transport.ready()).thenReturn(true);
        long adminId = identities.findByUsername("admin").orElseThrow().id();
        long projectId = assets.createProject("EDGE_OTHER_" + UUID.randomUUID(),
            "另一原生接入项目", adminId);
        fixtureProjects.add(projectId);
        long siteId = assets.createSite(projectId, "EDGE_OTHER_SITE_" + UUID.randomUUID(),
            "另一原生接入站点", "Asia/Shanghai", adminId);
        String otherMachineCode = "GW-NATIVE-OTHER-" + UUID.randomUUID().toString().substring(0, 8);
        assets.createDevice(siteId, otherMachineCode, "另一原生接入设备", "EDGE-GW", "MANUAL", adminId);

        MvcResult subscribed = mockMvc.perform(post(
                "/api/v2/devices/{machineCode}/realtime/subscriptions", machineCode)
                .with(csrf()).contentType("application/json")
                .content("""
                    {"meterCode":"PCS_1","intervalMs":1000,"ttlSec":30}
                    """))
            .andExpect(status().isAccepted())
            .andReturn();
        String sessionId = objectMapper.readTree(subscribed.getResponse().getContentAsString())
            .path("data").path("sessionId").asText();
        clearInvocations(transport);

        mockMvc.perform(delete(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}",
                otherMachineCode, sessionId).with(csrf()))
            .andExpect(status().isBadRequest());
        mockMvc.perform(delete(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}",
                machineCode, "MONITOR_test-session").with(csrf()))
            .andExpect(status().isBadRequest());
        mockMvc.perform(delete(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}",
                machineCode, "REALTIME_unknown-session").with(csrf()))
            .andExpect(status().isBadRequest());

        verify(transport, never()).publish(any(), any(byte[].class), anyInt());

        mockMvc.perform(delete(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}",
                machineCode, sessionId).with(csrf()))
            .andExpect(status().isAccepted());
        verify(transport).publish(eq("edge/telemetry/realtime/request/" + machineCode),
            argThat(bytes -> new String(bytes, java.nio.charset.StandardCharsets.UTF_8)
                .contains("\"action\":\"unsubscribe\"")), eq(2));
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void realtimeLeaseSurvivesServiceInstanceReplacementAndStopFailureCanRetry() throws Exception {
        when(transport.ready()).thenReturn(true);
        Authentication authentication = SecurityContextHolder.getContext().getAuthentication();
        MvcResult subscribed = mockMvc.perform(post(
                "/api/v2/devices/{machineCode}/realtime/subscriptions", machineCode)
                .with(csrf()).contentType("application/json")
                .content("""
                    {"meterCode":"PCS_1","intervalMs":1000,"ttlSec":30}
                    """))
            .andExpect(status().isAccepted())
            .andReturn();
        String sessionId = objectMapper.readTree(subscribed.getResponse().getContentAsString())
            .path("data").path("sessionId").asText();
        clearInvocations(transport);

        doThrow(new EdgeTransportException("injected stop failure", null, false))
            .doNothing()
            .when(transport).publish(eq("edge/telemetry/realtime/request/" + machineCode),
                argThat(bytes -> new String(bytes, java.nio.charset.StandardCharsets.UTF_8)
                    .contains("\"action\":\"unsubscribe\"")), eq(2));

        var replacement = new EdgeRuntimeService(
            devices, edgeRuntimeRepository, objectMapper, operations);
        SecurityContextHolder.getContext().setAuthentication(authentication);
        assertThrows(EdgeTransportException.class,
            () -> replacement.unsubscribeRealtime(machineCode, sessionId));
        assertTrue(edgeRuntimeRepository.realtimeLease(sessionId).orElseThrow()
            .state().equals("STOP_FAILED"));

        var retryingInstance = new EdgeRuntimeService(
            devices, edgeRuntimeRepository, objectMapper, operations);
        SecurityContextHolder.getContext().setAuthentication(authentication);
        retryingInstance.unsubscribeRealtime(machineCode, sessionId);
        assertTrue(edgeRuntimeRepository.realtimeLease(sessionId).isEmpty());
        verify(transport, org.mockito.Mockito.times(2)).publish(
            eq("edge/telemetry/realtime/request/" + machineCode), any(byte[].class), eq(2));
        verify(transport, never()).publish(argThat(topic ->
            topic != null && topic.startsWith("edge/telemetry/full/")),
            any(byte[].class), anyInt());
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void realtimeSubscribePublishFailureRemovesPersistedLease() throws Exception {
        when(transport.ready()).thenReturn(true);
        doThrow(new EdgeTransportException("injected subscribe failure", null, false))
            .when(transport).publish(eq("edge/telemetry/realtime/request/" + machineCode),
                argThat(bytes -> new String(bytes, java.nio.charset.StandardCharsets.UTF_8)
                    .contains("\"action\":\"subscribe\"")), eq(2));

        mockMvc.perform(post(
                "/api/v2/devices/{machineCode}/realtime/subscriptions", machineCode)
                .with(csrf()).contentType("application/json")
                .content("""
                    {"meterCode":"PCS_1","intervalMs":1000,"ttlSec":30}
                    """))
            .andExpect(status().isServiceUnavailable());

        Long leaseCount = edgeRuntimeRepository.findDeviceId(machineCode)
            .map(deviceId -> edgeRuntimeRepository.realtimeLeaseCount(deviceId))
            .orElseThrow();
        assertTrue(leaseCount == 0L);
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void uncertainRealtimeSubscribeReturnsACompensatableSession(boolean unknownFailure) throws Exception {
        when(transport.ready()).thenReturn(true);
        RuntimeException failure = unknownFailure
            ? new IllegalStateException("injected unknown publish outcome")
            : new EdgeTransportException("injected publish acknowledgement timeout");
        doThrow(failure)
            .when(transport).publish(eq("edge/telemetry/realtime/request/" + machineCode),
                argThat(bytes -> new String(bytes, java.nio.charset.StandardCharsets.UTF_8)
                    .contains("\"action\":\"subscribe\"")), eq(2));

        MvcResult subscribed = mockMvc.perform(post(
                "/api/v2/devices/{machineCode}/realtime/subscriptions", machineCode)
                .with(csrf()).contentType("application/json")
                .content("""
                    {"meterCode":"PCS_1","intervalMs":1000,"ttlSec":30}
                    """))
            .andExpect(status().isAccepted())
            .andExpect(jsonPath("$.data.sessionId",
                org.hamcrest.Matchers.startsWith("REALTIME_")))
            .andReturn();
        String sessionId = objectMapper.readTree(subscribed.getResponse().getContentAsString())
            .path("data").path("sessionId").asText();

        var lease = edgeRuntimeRepository.realtimeLease(sessionId).orElseThrow();
        assertTrue(lease.state().equals("STARTING"));

        mockMvc.perform(delete(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}",
                machineCode, sessionId).with(csrf()))
            .andExpect(status().isAccepted());
        assertTrue(edgeRuntimeRepository.realtimeLease(sessionId).isEmpty());
        verify(transport, never()).publish(argThat(topic ->
            topic != null && topic.startsWith("edge/telemetry/full/")),
            any(byte[].class), anyInt());
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void realtimeSubscribeNeverPromotesOnDeliveryAlone() throws Exception {
        when(transport.ready()).thenReturn(true);
        doThrow(new IllegalStateException("injected ACTIVE persistence failure"))
            .when(edgeRuntimeRepository).markRealtimeLeaseActive(anyString(), anyLong());

        MvcResult subscribed = mockMvc.perform(post(
                "/api/v2/devices/{machineCode}/realtime/subscriptions", machineCode)
                .with(csrf()).contentType("application/json")
                .content("""
                    {"meterCode":"PCS_1","intervalMs":1000,"ttlSec":30}
                    """))
            .andExpect(status().isAccepted())
            .andExpect(jsonPath("$.data.sessionId",
                org.hamcrest.Matchers.startsWith("REALTIME_")))
            .andReturn();
        String sessionId = objectMapper.readTree(subscribed.getResponse().getContentAsString())
            .path("data").path("sessionId").asText();

        assertTrue(edgeRuntimeRepository.realtimeLease(sessionId).orElseThrow()
            .state().equals("STARTING"));
        verify(edgeRuntimeRepository, never()).markRealtimeLeaseActive(anyString(), anyLong());
        verify(transport).publish(eq("edge/telemetry/realtime/request/" + machineCode),
            argThat(bytes -> new String(bytes, java.nio.charset.StandardCharsets.UTF_8)
                .contains("\"action\":\"subscribe\"")), eq(2));

        mockMvc.perform(delete(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}",
                machineCode, sessionId).with(csrf()))
            .andExpect(status().isAccepted());
        assertTrue(edgeRuntimeRepository.realtimeLease(sessionId).isEmpty());
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void realtimeSubscribeAndStopResponsesSurviveAuditWriteFailure() throws Exception {
        when(transport.ready()).thenReturn(true);
        AtomicInteger failedWrites = new AtomicInteger();
        doAnswer(invocation -> {
            failedWrites.incrementAndGet();
            throw new IllegalStateException("injected audit failure");
        }).when(audit).record(any(), anyString(), anyString(), anyString(), anyString(),
            anyString(), anyMap(), any());

        MvcResult subscribed = mockMvc.perform(post(
                "/api/v2/devices/{machineCode}/realtime/subscriptions", machineCode)
                .with(csrf()).contentType("application/json")
                .content("""
                    {"meterCode":"PCS_1","intervalMs":1000,"ttlSec":30}
                    """))
            .andExpect(status().isAccepted())
            .andReturn();
        String sessionId = objectMapper.readTree(subscribed.getResponse().getContentAsString())
            .path("data").path("sessionId").asText();

        mockMvc.perform(delete(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}",
                machineCode, sessionId).with(csrf()))
            .andExpect(status().isAccepted())
            .andExpect(jsonPath("$.data.sessionId").value(sessionId));

        assertEquals(2, failedWrites.get());
        assertTrue(edgeRuntimeRepository.realtimeLease(sessionId).isEmpty());
        verify(transport, never()).publish(argThat(topic ->
            topic != null && topic.startsWith("edge/telemetry/full/")),
            any(byte[].class), anyInt());
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void uncertainRealtimeStopBecomesRetryableAndRepublishes() throws Exception {
        when(transport.ready()).thenReturn(true);
        MvcResult subscribed = mockMvc.perform(post(
                "/api/v2/devices/{machineCode}/realtime/subscriptions", machineCode)
                .with(csrf()).contentType("application/json")
                .content("""
                    {"meterCode":"PCS_1","intervalMs":1000,"ttlSec":30}
                    """))
            .andExpect(status().isAccepted())
            .andReturn();
        String sessionId = objectMapper.readTree(subscribed.getResponse().getContentAsString())
            .path("data").path("sessionId").asText();
        clearInvocations(transport);

        doThrow(new EdgeTransportException("injected stop acknowledgement timeout"))
            .doNothing()
            .when(transport).publish(eq("edge/telemetry/realtime/request/" + machineCode),
                argThat(bytes -> new String(bytes, java.nio.charset.StandardCharsets.UTF_8)
                    .contains("\"action\":\"unsubscribe\"")), eq(2));

        mockMvc.perform(delete(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}",
                machineCode, sessionId).with(csrf()))
            .andExpect(status().isServiceUnavailable());
        assertTrue(edgeRuntimeRepository.realtimeLease(sessionId).orElseThrow()
            .state().equals("STOP_FAILED"));

        mockMvc.perform(delete(
                "/api/v2/devices/{machineCode}/realtime/subscriptions/{sessionId}",
                machineCode, sessionId).with(csrf()))
            .andExpect(status().isAccepted());
        assertTrue(edgeRuntimeRepository.realtimeLease(sessionId).isEmpty());
        verify(transport, org.mockito.Mockito.times(2)).publish(
            eq("edge/telemetry/realtime/request/" + machineCode),
            any(byte[].class), eq(2));
        verify(transport, never()).publish(argThat(topic ->
            topic != null && topic.startsWith("edge/telemetry/full/")),
            any(byte[].class), anyInt());
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void stoppingClaimSuppressesDuplicateStopAndFailedClaimCanRetry() {
        when(transport.ready()).thenReturn(true);
        var device = devices.deviceRef(machineCode);
        String sessionId = "REALTIME_" + UUID.randomUUID();
        edgeRuntimeRepository.createRealtimeLease(
            sessionId, device.id(), null, 1000, Instant.now().plusSeconds(30),
            identities.findByUsername("admin").orElseThrow().id());
        assertTrue(edgeRuntimeRepository.markRealtimeLeaseActive(sessionId, device.id()) == 1);
        String firstClaim = UUID.randomUUID().toString();
        assertTrue(edgeRuntimeRepository.claimRealtimeLeaseForStop(
            sessionId, device.id(), firstClaim) == 1);
        assertTrue(edgeRuntimeRepository.claimRealtimeLeaseForStop(
            sessionId, device.id(), UUID.randomUUID().toString()) == 0);
        assertTrue(edgeRuntimeRepository.deleteStoppingRealtimeLease(
            sessionId, device.id(), "wrong-claim") == 0);
        assertTrue(edgeRuntimeRepository.markRealtimeLeaseStopFailed(
            sessionId, device.id(), "wrong-claim") == 0);

        new EdgeRuntimeService(devices, edgeRuntimeRepository, objectMapper, operations)
            .unsubscribeRealtime(machineCode, sessionId);
        verify(transport, never()).publish(any(), any(byte[].class), anyInt());

        assertTrue(edgeRuntimeRepository.markRealtimeLeaseStopFailed(
            sessionId, device.id(), firstClaim) == 1);
        new EdgeRuntimeService(devices, edgeRuntimeRepository, objectMapper, operations)
            .unsubscribeRealtime(machineCode, sessionId);
        assertTrue(edgeRuntimeRepository.realtimeLease(sessionId).isEmpty());
        verify(transport).publish(eq("edge/telemetry/realtime/request/" + machineCode),
            argThat(bytes -> new String(bytes, java.nio.charset.StandardCharsets.UTF_8)
                .contains("\"action\":\"unsubscribe\"")), eq(2));
        verify(transport, never()).publish(argThat(topic ->
            topic != null && topic.startsWith("edge/telemetry/full/")),
            any(byte[].class), anyInt());
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void realtimeLeaseCleanupRemovesOnlyExpiredRetentionRecords() {
        var device = devices.deviceRef(machineCode);
        String expired = "REALTIME_" + UUID.randomUUID();
        String current = "REALTIME_" + UUID.randomUUID();
        edgeRuntimeRepository.createRealtimeLease(
            expired, device.id(), null, 1000, Instant.now().minusSeconds(360),
            identities.findByUsername("admin").orElseThrow().id());
        edgeRuntimeRepository.createRealtimeLease(
            current, device.id(), null, 1000, Instant.now().plusSeconds(30),
            identities.findByUsername("admin").orElseThrow().id());

        new EdgeRuntimeService(devices, edgeRuntimeRepository, objectMapper, operations)
            .cleanupExpiredRealtimeLeases();

        assertTrue(edgeRuntimeRepository.realtimeLease(expired).isEmpty());
        assertTrue(edgeRuntimeRepository.realtimeLease(current).isPresent());
    }

    @Test
    @WithUserDetails("admin")
    void realtimeLeaseRejectsUnsafeOrOutOfRangeInputBeforeMqttPublish() throws Exception {
        when(transport.ready()).thenReturn(true);

        mockMvc.perform(post("/api/v2/devices/{machineCode}/realtime/subscriptions", machineCode)
                .with(csrf()).contentType("application/json")
                .content("""
                    {"meterCode":"PCS/#","intervalMs":10,"ttlSec":3600}
                    """))
            .andExpect(status().isBadRequest());
    }

    @Test
    @WithUserDetails("admin")
    void nativeSystemTelemetryProvidesDeviceHealthFacts() throws Exception {
        ingress.ingest("edge/system/monitor/telemetry/" + machineCode, """
            {"type":"system-monitor","machineCode":"%s","cpuUsage":12.4,
             "memUsage":36.8,"diskUsage":41.2,"load1":0.52,"processCount":113,
             "cellular":{"enabled":true,"present":true,"registered":true,
               "connected":true,"interfaceName":"usb0","signalPercent":76,
               "route":{"available":true,"failoverEnabled":true,
                 "preferCellular":true,"usingCellular":true,"mode":"cellular",
                 "activeInterface":"usb0","lastResult":"healthy",
                 "lastChecked":"2026-07-30T06:00:00Z","ignored":"do-not-store"}},
             "services":[{"name":"MqttDriver","running":true}],"ts":1780000001500}
            """.formatted(machineCode));

        mockMvc.perform(get("/api/v2/devices/{machineCode}/system-monitor/telemetry", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.source").value("NATIVE_MQTT"))
            .andExpect(jsonPath("$.data.cpuUsage").value(12.4))
            .andExpect(jsonPath("$.data.memUsage").value(36.8))
            .andExpect(jsonPath("$.data.cellular.route.failoverEnabled").value(true))
            .andExpect(jsonPath("$.data.cellular.route.preferCellular").value(true))
            .andExpect(jsonPath("$.data.cellular.route.usingCellular").value(true))
            .andExpect(jsonPath("$.data.cellular.route.activeInterface").value("usb0"))
            .andExpect(jsonPath("$.data.cellular.route.ignored").doesNotExist())
            .andExpect(jsonPath("$.data.services[0].running").value(true));
    }

    @Test
    @WithUserDetails("admin")
    void nativeCameraStatusBuildsTheMediaDirectoryAndRedactsUrlCredentials() throws Exception {
        ingress.ingest("edge/camera/status/" + machineCode, """
            {"machineCode":"%s","cameraCode":"CAM001","online":true,"fps":15,
             "bitrateKbps":1480,"streamUrl":"rtsp://camera-user:camera-secret@media.local/%s/CAM001",
             "ts":1780000002000}
            """.formatted(machineCode, machineCode));
        ingress.ingest("edge/camera/status/" + machineCode, """
            {"machineCode":"%s","cameraCode":"CAM002","online":false,"fps":0,
             "bitrateKbps":0,"ts":1780000002001}
            """.formatted(machineCode));

        mockMvc.perform(get("/api/v2/devices/{machineCode}/media/streams", machineCode))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.source").value("NATIVE_MQTT"))
            .andExpect(jsonPath("$.data.streams.length()").value(2))
            .andExpect(jsonPath("$.data.streams[0].cameraCode").value("CAM001"))
            .andExpect(jsonPath("$.data.streams[0].online").value(true))
            .andExpect(jsonPath("$.data.streams[0].streamUrl").value(
                "rtsp://***:***@media.local/" + machineCode + "/CAM001"))
            .andExpect(content().string(not(containsString("camera-secret"))));
    }

    @Test
    @WithUserDetails("admin")
    void nativeAlarmTopicUsesTheSameDurableAlarmWorkflow() throws Exception {
        ingress.ingest("edge/alarm/" + machineCode, """
            {"type":"alarm","machineCode":"%s","meterCode":"PCS_1","index":110009,
             "pointCode":"DC_OVER_VOLTAGE","alarmType":"high","active":true,
             "value":812.4,"quality":1,"ts":1780000003000,"stale":false}
            """.formatted(machineCode));

        mockMvc.perform(get("/api/v2/alarms").param("view", "ACTIVE"))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.total").value(1))
            .andExpect(jsonPath("$.data.items[0].machineCode").value(machineCode))
            .andExpect(jsonPath("$.data.items[0].alarmCode").value("high"))
            .andExpect(jsonPath("$.data.items[0].severity").value("UNKNOWN"));
    }

    @Test
    void mqttCallbackNormalizesSystemAlertWithoutAnHttpSecurityContext() throws Exception {
        ingress.ingest("edge/system/monitor/alert/" + machineCode, """
            {"type":"system-alert","machineCode":"%s","metric":"cpuUsage",
             "level":"critical","value":92.5,"threshold":90,"active":true,
             "message":"cpu usage too high","ts":1780000003500}
            """.formatted(machineCode));

        mockMvc.perform(get("/api/v2/alarms").param("view", "ACTIVE")
                .with(user(new PlatformPrincipal(identities.findByUsername("admin").orElseThrow()))))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.total").value(1))
            .andExpect(jsonPath("$.data.items[0].alarmCode").value("cpuUsage"))
            .andExpect(jsonPath("$.data.items[0].severity").value("CRITICAL"))
            .andExpect(jsonPath("$.data.items[0].message").value("cpu usage too high"));
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void remoteDiagnosticPublishesDirectlyAndCompletesFromTheNativeReplyTopic() throws Exception {
        when(transport.ready()).thenReturn(true);
        MvcResult created = mockMvc.perform(post("/api/v2/tasks").with(csrf())
                .header("Idempotency-Key", "native-diag-request-001")
                .contentType("application/json")
                .content("""
                    {"machineCode":"%s","commandName":"df_root","tailLines":100}
                    """.formatted(machineCode)))
            .andExpect(status().isAccepted()).andReturn();
        String taskId = objectMapper.readTree(created.getResponse().getContentAsString())
            .path("data").path("id").asText();

        worker().runOnce();
        String cmdId = tasks.findWorkItems(com.gatewayzk.platform.task.TaskStatus.DISPATCHED,
                "REMOTE_DIAGNOSTIC", 100).stream()
            .filter(item -> item.publicId().equals(taskId)).findFirst().orElseThrow().externalReference();
        var binding = edgeRuntimeRepository.diagnosticRequest(cmdId).orElseThrow();
        ingress.ingest("edge/system/diag/reply/" + machineCode, """
            {"replyKind":"terminal","cmdId":"%s","machineCode":"%s","operator":"%s",
             "command":"df_root","arg":"","success":true,"exitCode":0,"stdout":"disk ok",
             "stderr":"","requestTs":%d,"ts":1780000004100}
            """.formatted(cmdId, machineCode, binding.operator(), binding.requestTs()));
        worker().runOnce();

        mockMvc.perform(get("/api/v2/tasks/{id}", taskId))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.summary.status").value("SUCCEEDED"))
            .andExpect(jsonPath("$.data.targets[0].resultCode").value("DIAGNOSTIC_SUCCEEDED"))
            .andExpect(jsonPath("$.data.targets[0].result.reason").doesNotExist())
            .andExpect(jsonPath("$.data.targets[0].result.cleanupConfirmed").doesNotExist())
            .andExpect(content().string(not(containsString("LEGACY_"))));
    }

    @Test
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void diagnosticReplyIsRedactedBeforeItReachesTheDurableInbox() {
        String cmdId = "DIAG_REDACT_001";
        edgeRuntimeRepository.reserveDiagnosticRequest(cmdId,
            edgeRuntimeRepository.findDeviceId(machineCode).orElseThrow(), "df_root", "", "fixture", 1780000004000L);
        ingress.ingest("edge/system/diag/reply/" + machineCode, """
            {"replyKind":"terminal","cmdId":"%s","machineCode":"%s","command":"df_root",
             "arg":"","operator":"fixture",
             "success":true,"exitCode":0,
             "stdout":"password=durable-secret-value disk ok",
             "stderr":"Bearer durable-token-value","requestTs":1780000004000,
             "ts":1780000004100}
            """.formatted(cmdId, machineCode));

        String stored = operations.findDiagnosticResult(machineCode, cmdId)
            .orElseThrow().toString();
        assertFalse(stored.contains("durable-secret-value"));
        assertFalse(stored.contains("durable-token-value"));
        assertTrue(stored.contains("[REDACTED]"));
        assertTrue(stored.contains("\"redacted\":true"));
    }

    @ParameterizedTest
    @ValueSource(booleans = {true, false})
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void conflictingAttemptNeverOccupiesTheOriginalTerminalInbox(boolean conflictFirst) throws Exception {
        when(transport.ready()).thenReturn(true);
        String taskId = createDiagnosticTask("df_root");
        worker().runOnce();
        String cmdId = diagnosticCmdId(taskId);
        var binding = edgeRuntimeRepository.diagnosticRequest(cmdId).orElseThrow();
        String terminal = terminalReply(cmdId, binding, "df_root", "disk ok");
        String conflict = """
            {"replyKind":"admission_rejection","attemptedCmdId":"%s",
             "machineCode":"%s","command":"uptime","arg":"","operator":"other",
             "reason":"cmdId_conflict"}
            """.formatted(cmdId, machineCode);
        String wrong = terminalReply(cmdId, binding, "uptime", "wrong");
        String topic = "edge/system/diag/reply/" + machineCode;
        if (conflictFirst) {
            ingress.ingest(topic, conflict);
            assertThrows(EdgeMessageRejectedException.class, () -> ingress.ingest(topic, wrong));
            assertTrue(operations.findDiagnosticResult(machineCode, cmdId).isEmpty());
        }
        ingress.ingest(topic, terminal);
        if (!conflictFirst) {
            ingress.ingest(topic, conflict);
            assertThrows(EdgeMessageRejectedException.class, () -> ingress.ingest(topic, wrong));
        }
        ingress.ingest(topic, terminal);
        assertTrue(operations.findDiagnosticResult(machineCode, cmdId).orElseThrow()
            .path("stdout").asText().equals("disk ok"));
        worker().runOnce();
        mockMvc.perform(get("/api/v2/tasks/{id}", taskId)).andExpect(status().isOk())
            .andExpect(jsonPath("$.data.summary.status").value("SUCCEEDED"))
            .andExpect(jsonPath("$.data.targets[0].result.stdout").value("disk ok"));
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void wrongFingerprintCannotBlockTheRightTerminalAndFastReplyCanPrecedeDispatchRecord() throws Exception {
        when(transport.ready()).thenReturn(true);
        String taskId = createDiagnosticTask("df_root");
        var fastCmdId = new java.util.concurrent.atomic.AtomicReference<String>();
        doAnswer(invocation -> {
            var request = objectMapper.readTree((byte[]) invocation.getArgument(1));
            String cmdId = request.path("cmdId").asText();
            fastCmdId.set(cmdId);
            assertTrue(tasks.findWorkItems(com.gatewayzk.platform.task.TaskStatus.DISPATCHING,
                "REMOTE_DIAGNOSTIC", 100).stream().anyMatch(item -> item.publicId().equals(taskId)));
            assertTrue(java.util.concurrent.CompletableFuture.supplyAsync(() ->
                edgeRuntimeRepository.diagnosticRequest(cmdId).isPresent()).get(2, TimeUnit.SECONDS));
            var binding = edgeRuntimeRepository.diagnosticRequest(cmdId).orElseThrow();
            ingress.ingest("edge/system/diag/reply/" + machineCode,
                terminalReply(cmdId, binding, "df_root", "fast result"));
            return null;
        }).when(transport).publish(anyString(), any(byte[].class), anyInt());
        worker().runOnce();
        String cmdId = fastCmdId.get();
        assertTrue(cmdId != null);
        assertTrue(operations.findDiagnosticResult(machineCode, cmdId).isPresent());
        worker().runOnce();
        mockMvc.perform(get("/api/v2/tasks/{id}", taskId)).andExpect(status().isOk())
            .andExpect(jsonPath("$.data.summary.status").value("SUCCEEDED"))
            .andExpect(jsonPath("$.data.targets[0].result.stdout").value("fast result"));
    }

    @Test
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void typedClassificationAndTruncatedFakeKeyAreSanitizedAtActualIngress() throws Exception {
        String cmdId = "DIAG_TYPED_" + UUID.randomUUID();
        long deviceId = edgeRuntimeRepository.findDeviceId(machineCode).orElseThrow();
        edgeRuntimeRepository.reserveDiagnosticRequest(cmdId, deviceId, "df_root", "", "fixture", 1780000004000L);
        var binding = edgeRuntimeRepository.diagnosticRequest(cmdId).orElseThrow();
        var terminal = (com.fasterxml.jackson.databind.node.ObjectNode) objectMapper.readTree(
            terminalReply(cmdId, binding, "df_root", "safe"));
        String topic = "edge/system/diag/reply/" + machineCode;
        for (String field : new String[] {"exitCode", "signal", "requestTs"}) {
            var invalid = terminal.deepCopy();
            invalid.put(field, 1.5);
            assertThrows(EdgeMessageRejectedException.class, () ->
                ingress.ingest(topic, invalid.toString()));
        }
        for (String invalidReason : new String[] {"password=leak", "timeout"}) {
            var invalid = terminal.deepCopy();
            invalid.put("reason", invalidReason);
            assertThrows(EdgeMessageRejectedException.class, () ->
                ingress.ingest(topic, invalid.toString()));
        }
        var invalidSuccess = terminal.deepCopy();
        invalidSuccess.put("success", false);
        assertThrows(EdgeMessageRejectedException.class, () ->
            ingress.ingest(topic, invalidSuccess.toString()));
        assertTrue(operations.findDiagnosticResult(machineCode, cmdId).isEmpty());

        terminal.put("stdout", "-----BEGIN PRIVATE KEY-----\nFAKE_KEY_MATERIAL_123\n" +
            "x".repeat(131_100));
        terminal.put("truncated", true);
        terminal.put("reason", "exit");
        terminal.put("signal", 0);
        terminal.put("timedOut", false);
        terminal.put("cleanupConfirmed", true);
        ingress.ingest(topic, terminal.toString());
        String stored = operations.findDiagnosticResult(machineCode, cmdId).orElseThrow().toString();
        assertFalse(stored.contains("FAKE_KEY_MATERIAL_123"));
        assertTrue(stored.contains("[REDACTED_PRIVATE_KEY]"));
        assertTrue(stored.contains("\"truncated\":true"));
        assertTrue(stored.contains("\"cleanupConfirmed\":true"));
    }

    @Test
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void longUnicodeOperatorUsesOneByteBoundedPublishedAndPersistedIdentity() throws Exception {
        when(transport.ready()).thenReturn(true);
        String longOperator = "中".repeat(50);
        String cmdId = operations.executeDiagnostic(machineCode, longOperator, "df_root", null, 100);
        var binding = edgeRuntimeRepository.diagnosticRequest(cmdId).orElseThrow();
        assertEquals("中".repeat(40), binding.operator());
        assertEquals(120, binding.operator().getBytes(java.nio.charset.StandardCharsets.UTF_8).length);
        var published = org.mockito.ArgumentCaptor.forClass(byte[].class);
        verify(transport).publish(eq("edge/system/diag/request/" + machineCode), published.capture(), eq(2));
        assertEquals(binding.operator(), objectMapper.readTree(published.getValue()).path("operator").asText());
        ingress.ingest("edge/system/diag/reply/" + machineCode,
            terminalReply(cmdId, binding, "df_root", "unicode ok"));
        assertEquals("unicode ok", operations.findDiagnosticResult(machineCode, cmdId)
            .orElseThrow().path("stdout").asText());
    }

    @Test
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void diagnosticOnlyIngressAcceptsBoundedEscapedOutputAboveOrdinaryLimit() {
        String cmdId = "DIAG_LARGE_" + UUID.randomUUID();
        edgeRuntimeRepository.reserveDiagnosticRequest(cmdId,
            edgeRuntimeRepository.findDeviceId(machineCode).orElseThrow(),
            "df_root", "", "fixture", 1780000004000L);
        String payload = "{\"replyKind\":\"terminal\",\"cmdId\":\"" + cmdId +
            "\",\"machineCode\":\"" + machineCode +
            "\",\"command\":\"df_root\",\"arg\":\"\",\"operator\":\"fixture\"," +
            "\"success\":true,\"exitCode\":0,\"requestTs\":1780000004000," +
            "\"stdout\":\"" + "\\ufffd".repeat(700_000) + "\",\"stderr\":\"\"}";
        assertTrue(payload.length() > 4 * 1024 * 1024);
        assertTrue(payload.length() < EdgeMqttProperties.MAX_DIAGNOSTIC_REPLY_BYTES);
        ingress.ingest("edge/system/diag/reply/" + machineCode, payload);
        assertTrue(operations.findDiagnosticResult(machineCode, cmdId).orElseThrow()
            .path("truncated").asBoolean());
        assertThrows(EdgeMessageRejectedException.class, () ->
            ingress.ingest("edge/status/" + machineCode, payload));
    }

    @Test
    @WithUserDetails("admin")
    @Transactional(propagation = Propagation.NOT_SUPPORTED)
    void failureCategoriesSurviveIngressWorkerAndTaskApiWithoutInventingLegacyFields() throws Exception {
        when(transport.ready()).thenReturn(true);
        String[] reasons = {"timeout", "cleanup_unknown", "signal", "exit", "launch_failed"};
        for (String reason : reasons) {
            String taskId = createDiagnosticTask("df_root");
            worker().runOnce();
            String cmdId = diagnosticCmdId(taskId);
            var binding = edgeRuntimeRepository.diagnosticRequest(cmdId).orElseThrow();
            var reply = (com.fasterxml.jackson.databind.node.ObjectNode) objectMapper.readTree(
                terminalReply(cmdId, binding, "df_root", "category"));
            reply.put("success", false);
            reply.put("exitCode", "exit".equals(reason) ? 7 : -1);
            reply.put("reason", reason);
            reply.put("signal", "signal".equals(reason) ? 15 : 0);
            reply.put("timedOut", "timeout".equals(reason));
            reply.put("cleanupConfirmed", !"cleanup_unknown".equals(reason));
            reply.put("truncated", "timeout".equals(reason));
            ingress.ingest("edge/system/diag/reply/" + machineCode, reply.toString());
            worker().runOnce();
            mockMvc.perform(get("/api/v2/tasks/{id}", taskId)).andExpect(status().isOk())
                .andExpect(jsonPath("$.data.summary.status").value("FAILED"))
                .andExpect(jsonPath("$.data.targets[0].result.reason").value(reason))
                .andExpect(jsonPath("$.data.targets[0].result.signal")
                    .value("signal".equals(reason) ? 15 : 0))
                .andExpect(jsonPath("$.data.targets[0].result.cleanupConfirmed")
                    .value(!"cleanup_unknown".equals(reason)));
        }
    }

    private String createDiagnosticTask(String command) throws Exception {
        MvcResult created = mockMvc.perform(post("/api/v2/tasks").with(csrf())
                .header("Idempotency-Key", "native-diag-" + UUID.randomUUID())
                .contentType("application/json")
                .content("{\"machineCode\":\"" + machineCode +
                    "\",\"commandName\":\"" + command + "\",\"tailLines\":100}"))
            .andExpect(status().isAccepted()).andReturn();
        return objectMapper.readTree(created.getResponse().getContentAsString())
            .path("data").path("id").asText();
    }

    private String diagnosticCmdId(String taskId) {
        return tasks.findWorkItems(com.gatewayzk.platform.task.TaskStatus.DISPATCHED,
                "REMOTE_DIAGNOSTIC", 100).stream()
            .filter(item -> item.publicId().equals(taskId)).findFirst().orElseThrow().externalReference();
    }

    private String terminalReply(String cmdId, EdgeRuntimeRepository.DiagnosticRequestBinding binding,
                                 String command, String output) throws Exception {
        var reply = objectMapper.createObjectNode();
        reply.put("replyKind", "terminal");
        reply.put("cmdId", cmdId);
        reply.put("machineCode", machineCode);
        reply.put("command", command);
        reply.put("arg", binding.argument());
        reply.put("operator", binding.operator());
        reply.put("success", true);
        reply.put("exitCode", 0);
        reply.put("stdout", output);
        reply.put("stderr", "");
        reply.put("requestTs", binding.requestTs());
        reply.put("ts", binding.requestTs() + 1);
        return objectMapper.writeValueAsString(reply);
    }

    private TaskWorker worker() {
        return new TaskWorker(tasks, lifecycle, taskProperties, operations, sanitizer, objectMapper);
    }
}
