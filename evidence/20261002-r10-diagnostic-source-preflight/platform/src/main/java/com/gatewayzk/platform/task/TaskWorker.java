package com.gatewayzk.platform.task;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.gatewayzk.platform.edge.EdgeOperationsGateway;
import com.gatewayzk.platform.edge.EdgeTransportException;
import com.gatewayzk.platform.edge.NativeEdgeOperationsGateway;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.boot.autoconfigure.condition.ConditionalOnProperty;
import org.springframework.scheduling.annotation.Scheduled;
import org.springframework.stereotype.Component;

import java.time.Instant;

@Component
@ConditionalOnProperty(prefix = "platform.tasks",
    name = {"worker-enabled", "remote-diagnostic-worker-enabled"},
    havingValue = "true", matchIfMissing = true)
public class TaskWorker {

    private static final Logger log = LoggerFactory.getLogger(TaskWorker.class);

    private final TaskJdbcRepository tasks;
    private final TaskLifecycleService lifecycle;
    private final TaskEngineProperties properties;
    private final EdgeOperationsGateway edge;
    private final DiagnosticResultSanitizer sanitizer;
    private final ObjectMapper objectMapper;

    public TaskWorker(TaskJdbcRepository tasks, TaskLifecycleService lifecycle,
                      TaskEngineProperties properties, EdgeOperationsGateway edge,
                      DiagnosticResultSanitizer sanitizer, ObjectMapper objectMapper) {
        this.tasks = tasks;
        this.lifecycle = lifecycle;
        this.properties = properties;
        this.edge = edge;
        this.sanitizer = sanitizer;
        this.objectMapper = objectMapper;
    }

    @Scheduled(fixedDelayString = "${platform.tasks.poll-delay-ms:2000}")
    public void runOnce() {
        dispatchQueued();
        reconcileDispatched();
        expireOverdue();
    }

    private void dispatchQueued() {
        for (TaskJdbcRepository.TaskWorkItem queued
            : tasks.findWorkItems(TaskStatus.QUEUED, "REMOTE_DIAGNOSTIC", properties.batchSize())) {
            Instant deadline = Instant.now().plus(properties.remoteDiagnostic().timeout());
            if (!lifecycle.claim(queued, deadline)) continue;
            TaskJdbcRepository.TaskWorkItem claimed = tasks.findWorkItem(queued.taskId()).orElse(null);
            if (claimed == null) continue;
            try {
                JsonNode parameters = objectMapper.readTree(claimed.parametersJson());
                String commandId = edge.executeDiagnostic(claimed.machineCode(),
                    claimed.requestedByName(), requiredText(parameters, "commandName"),
                    optionalText(parameters, "service"), parameters.path("tailLines").asInt(100));
                if (commandId.length() > 128) throw new IllegalArgumentException("cmdId 超出长度限制");
                if (!lifecycle.markDispatched(claimed, commandId,
                    Instant.now().plus(properties.remoteDiagnostic().timeout()))) {
                    log.warn("Task {} was not in DISPATCHING state after native MQTT dispatch", claimed.publicId());
                }
            } catch (EdgeTransportException exception) {
                failDispatch(claimed, "EDGE_MQTT_UNAVAILABLE", "边端 MQTT 诊断链路当前不可用", true);
            } catch (JsonProcessingException | IllegalArgumentException exception) {
                failDispatch(claimed, "EDGE_DIAGNOSTIC_REQUEST_INVALID", "诊断请求无法投递", false);
            } catch (RuntimeException exception) {
                log.error("Unexpected diagnostic dispatch failure for task {}", claimed.publicId(), exception);
                failDispatch(claimed, "EDGE_DIAGNOSTIC_DISPATCH_FAILED", "诊断命令投递失败", true);
            }
        }
    }

    private void reconcileDispatched() {
        for (TaskJdbcRepository.TaskWorkItem item
            : tasks.findWorkItems(TaskStatus.DISPATCHED, "REMOTE_DIAGNOSTIC", properties.batchSize())) {
            try {
                JsonNode audit = edge.findDiagnosticResult(item.machineCode(), item.externalReference()).orElse(null);
                if (audit == null) continue;
                if (!matchesDiagnosticTask(item, audit)) {
                    log.warn("Ignoring diagnostic reply inconsistent with task {}", item.publicId());
                    continue;
                }
                DiagnosticResultSanitizer.SanitizedDiagnostic result = sanitizer.sanitize(audit);
                if (!audit.hasNonNull("success")) {
                    lifecycle.markFailed(item, "DIAGNOSTIC_RESULT_INVALID",
                        "设备诊断结果缺少成功状态", false, result.resultJson());
                } else if (audit.path("success").asBoolean()) {
                    lifecycle.markSucceeded(item, "DIAGNOSTIC_SUCCEEDED", result.resultJson());
                } else {
                    String code = audit.hasNonNull("exitCode")
                        ? "DIAGNOSTIC_EXIT_" + audit.path("exitCode").asInt()
                        : "DIAGNOSTIC_FAILED";
                    lifecycle.markFailed(item, code, "设备诊断命令执行失败", false, result.resultJson());
                }
            } catch (EdgeTransportException exception) {
                log.debug("Native edge result store unavailable while reconciling task {}", item.publicId());
            } catch (JsonProcessingException exception) {
                log.warn("Invalid diagnostic task parameters for {}", item.publicId(), exception);
            } catch (RuntimeException exception) {
                log.error("Unexpected diagnostic reconciliation failure for task {}", item.publicId(), exception);
            }
        }
    }

    private void expireOverdue() {
        for (TaskJdbcRepository.TaskWorkItem item
            : tasks.findExpired(java.util.Set.of("REMOTE_DIAGNOSTIC"), properties.batchSize(), Instant.now())) {
            try {
                lifecycle.markTimedOut(item);
            } catch (RuntimeException exception) {
                log.error("Could not expire task {}", item.publicId(), exception);
            }
        }
    }

    private boolean matchesDiagnosticTask(TaskJdbcRepository.TaskWorkItem item, JsonNode audit)
        throws JsonProcessingException {
        if (!item.machineCode().equals(audit.path("machineCode").asText()) ||
            "cmdId_conflict".equals(audit.path("reason").asText()) ||
            "admission_rejection".equals(audit.path("replyKind").asText())) return false;
        JsonNode parameters = objectMapper.readTree(item.parametersJson());
        String command = requiredText(parameters, "commandName");
        String arg = "systemctl_status".equals(command) ? optionalText(parameters, "service") :
            "journal_tail".equals(command) ? Integer.toString(parameters.path("tailLines").asInt(100)) : "";
        String operator = NativeEdgeOperationsGateway.normalizeDiagnosticOperator(item.requestedByName());
        boolean modern = "terminal".equals(audit.path("replyKind").asText());
        return (!modern || audit.hasNonNull("command") && audit.hasNonNull("arg") &&
                audit.hasNonNull("operator")) &&
            (!audit.hasNonNull("command") || command.equals(audit.path("command").asText())) &&
            (!audit.hasNonNull("arg") || arg.equals(audit.path("arg").asText())) &&
            (!audit.hasNonNull("operator") || operator.equals(audit.path("operator").asText()));
    }

    private void failDispatch(TaskJdbcRepository.TaskWorkItem item, String code,
                              String message, boolean retryable) {
        try {
            lifecycle.markFailed(item, code, message, retryable, "{}");
        } catch (RuntimeException exception) {
            log.error("Could not persist dispatch failure for task {}", item.publicId(), exception);
        }
    }

    private String requiredText(JsonNode node, String field) {
        String value = node == null ? "" : node.path(field).asText("").trim();
        if (value.isBlank()) throw new IllegalArgumentException(field + " 不能为空");
        return value;
    }

    private String optionalText(JsonNode node, String field) {
        String value = node == null ? "" : node.path(field).asText("").trim();
        return value.isBlank() ? null : value;
    }
}
