package com.gatewayzk.platform.task;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.gatewayzk.platform.asset.AssetJdbcRepository;
import com.gatewayzk.platform.identity.IdentityRepository;
import com.gatewayzk.platform.identity.ScopeAccess;
import com.gatewayzk.platform.edge.EdgeOperationsGateway;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.boot.test.autoconfigure.web.servlet.AutoConfigureMockMvc;
import org.springframework.boot.test.context.SpringBootTest;
import org.springframework.boot.test.mock.mockito.MockBean;
import org.springframework.security.test.context.support.WithUserDetails;
import org.springframework.test.context.ActiveProfiles;
import org.springframework.test.web.servlet.MockMvc;
import org.springframework.test.web.servlet.MvcResult;
import org.springframework.transaction.annotation.Transactional;

import java.util.Set;
import java.util.UUID;
import java.time.Instant;
import java.util.Optional;

import static org.assertj.core.api.Assertions.assertThat;
import static org.mockito.ArgumentMatchers.anyString;
import static org.mockito.ArgumentMatchers.eq;
import static org.mockito.Mockito.when;
import static org.springframework.security.test.web.servlet.request.SecurityMockMvcRequestPostProcessors.csrf;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.get;
import static org.springframework.test.web.servlet.request.MockMvcRequestBuilders.post;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.content;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.jsonPath;
import static org.springframework.test.web.servlet.result.MockMvcResultMatchers.status;

@SpringBootTest
@AutoConfigureMockMvc
@ActiveProfiles("test")
@Transactional
class TaskApiIntegrationTest {

    @Autowired private MockMvc mockMvc;
    @Autowired private ObjectMapper objectMapper;
    @Autowired private AssetJdbcRepository assets;
    @Autowired private IdentityRepository identities;
    @Autowired private TaskJdbcRepository tasks;
    @Autowired private TaskLifecycleService lifecycle;
    @Autowired private TaskEngineProperties properties;
    @Autowired private DiagnosticResultSanitizer sanitizer;

    @MockBean private EdgeOperationsGateway edge;

    private String machineCode;
    private long siteId;

    @BeforeEach
    void createDevice() {
        when(edge.ready()).thenReturn(true);
        String suffix = UUID.randomUUID().toString().substring(0, 8);
        long adminId = identities.findByUsername("admin").orElseThrow().id();
        long projectId = assets.createProject("TASK_" + suffix, "任务测试项目 " + suffix, adminId);
        siteId = assets.createSite(projectId, "SITE_" + suffix, "任务测试站点", "Asia/Shanghai", adminId);
        machineCode = "GW-TASK-" + suffix;
        assets.createDevice(siteId, machineCode, "任务测试网关", null, "TEST", adminId);
    }

    @Test
    @WithUserDetails("admin")
    void createsIdempotentlyAndRejectsKeyReuseForDifferentRequests() throws Exception {
        String key = "task-key-" + UUID.randomUUID();
        MvcResult created = create(key, "uptime").andExpect(status().isAccepted())
            .andExpect(jsonPath("$.data.created").value(true)).andReturn();
        String taskId = dataId(created);

        create(key, "uptime").andExpect(status().isOk())
            .andExpect(jsonPath("$.data.id").value(taskId))
            .andExpect(jsonPath("$.data.created").value(false));
        create(key, "free_m").andExpect(status().isConflict())
            .andExpect(jsonPath("$.error.code").value("IDEMPOTENCY_CONFLICT"));
        create("parallel-key-" + UUID.randomUUID(), "uptime").andExpect(status().isConflict())
            .andExpect(jsonPath("$.error.code").value("ACTIVE_TASK_CONFLICT"));

        mockMvc.perform(get("/api/v2/tasks/{taskId}", taskId))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.summary.status").value("QUEUED"))
            .andExpect(jsonPath("$.data.events.length()").value(2));
    }

    @Test
    @WithUserDetails("admin")
    void workerPersistsRealTerminalResultAndSanitizesOutput() throws Exception {
        when(edge.executeDiagnostic(eq(machineCode), anyString(), eq("uptime"), eq(null), eq(100)))
            .thenReturn("DIAG_TEST_1");
        when(edge.findDiagnosticResult(eq(machineCode), eq("DIAG_TEST_1"))).thenReturn(Optional.of(
            objectMapper.readTree("""
                {"cmdId":"DIAG_TEST_1","machineCode":"%s","success":true,"exitCode":0,
                 "stdoutText":"uptime ok token=must-not-survive","stderrText":"","replyTs":123}
                """.formatted(machineCode))));
        String taskId = dataId(create("worker-key-" + UUID.randomUUID(), "uptime")
            .andExpect(status().isAccepted()).andReturn());

        worker().runOnce();

        mockMvc.perform(get("/api/v2/tasks/{taskId}", taskId))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.summary.status").value("SUCCEEDED"))
            .andExpect(jsonPath("$.data.targets[0].progress").value(100))
            .andExpect(jsonPath("$.data.targets[0].result.redacted").value(true))
            .andExpect(jsonPath("$.data.events.length()").value(5))
            .andExpect(content().string(org.hamcrest.Matchers.not(
                org.hamcrest.Matchers.containsString("must-not-survive"))));
    }

    @Test
    @WithUserDetails("admin")
    void cancellationIsTerminalAndTaskQueriesRespectDeviceScope() throws Exception {
        String taskId = dataId(create("cancel-key-" + UUID.randomUUID(), "uptime")
            .andExpect(status().isAccepted()).andReturn());

        mockMvc.perform(post("/api/v2/tasks/{taskId}/cancel", taskId).with(csrf()))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.status").value("CANCELLED"));
        mockMvc.perform(post("/api/v2/tasks/{taskId}/cancel", taskId).with(csrf()))
            .andExpect(status().isConflict())
            .andExpect(jsonPath("$.error.code").value("INVALID_TASK_TRANSITION"));

        assertThat(tasks.listTasks(null, null, null, 50, null,
            new ScopeAccess(99L, false, Set.of(), Set.of(siteId))).items())
            .extracting(TaskSummaryView::id).contains(taskId);
        assertThat(tasks.listTasks(null, null, null, 50, null,
            new ScopeAccess(100L, false, Set.of(), Set.of())).items()).isEmpty();
    }

    @Test
    @WithUserDetails("admin")
    void validatesCommandSpecificArgumentsBeforeCreatingTask() throws Exception {
        create("systemctl-key-" + UUID.randomUUID(), "systemctl_status")
            .andExpect(status().isBadRequest())
            .andExpect(jsonPath("$.error.code").value("VALIDATION_ERROR"));
    }

    @Test
    @WithUserDetails("admin")
    void expiresAClaimLeftBehindByAStoppedWorkerWithoutRedispatching() throws Exception {
        String taskId = dataId(create("timeout-key-" + UUID.randomUUID(), "uptime")
            .andExpect(status().isAccepted()).andReturn());
        TaskJdbcRepository.TaskWorkItem queued = tasks.findWorkItems(TaskStatus.QUEUED, 100).stream()
            .filter(item -> item.publicId().equals(taskId)).findFirst().orElseThrow();

        assertThat(lifecycle.claim(queued, Instant.now().minusSeconds(1))).isTrue();
        TaskJdbcRepository.TaskWorkItem expired = tasks.findExpired(100, Instant.now()).stream()
            .filter(item -> item.publicId().equals(taskId)).findFirst().orElseThrow();
        assertThat(lifecycle.markTimedOut(expired)).isTrue();

        mockMvc.perform(get("/api/v2/tasks/{taskId}", taskId))
            .andExpect(status().isOk())
            .andExpect(jsonPath("$.data.summary.status").value("TIMED_OUT"))
            .andExpect(jsonPath("$.data.summary.retryable").value(true));
    }

    private org.springframework.test.web.servlet.ResultActions create(String key, String command)
        throws Exception {
        return mockMvc.perform(post("/api/v2/tasks").with(csrf())
            .header("Idempotency-Key", key)
            .contentType("application/json")
            .content(objectMapper.writeValueAsString(new TaskController.RemoteDiagnosticRequest(
                machineCode, command, null, 100))));
    }

    private String dataId(MvcResult result) throws Exception {
        JsonNode body = objectMapper.readTree(result.getResponse().getContentAsString());
        return body.path("data").path("id").asText();
    }

    private TaskWorker worker() {
        return new TaskWorker(tasks, lifecycle, properties, edge, sanitizer, objectMapper);
    }
}
