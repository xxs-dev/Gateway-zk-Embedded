package com.gatewayzk.platform.task;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import org.springframework.stereotype.Component;

import java.util.regex.Matcher;
import java.util.regex.Pattern;

@Component
public class DiagnosticResultSanitizer {

    private static final Pattern PRIVATE_KEY = Pattern.compile(
        "(?is)-----BEGIN [^-\\r\\n]*PRIVATE KEY-----.*?(?:-----END [^-\\r\\n]*PRIVATE KEY-----|\\z)");
    private static final Pattern BEARER_TOKEN = Pattern.compile(
        "(?i)\\bBearer\\s+[A-Za-z0-9._~+\\-/]+=*");
    private static final Pattern SECRET_ASSIGNMENT = Pattern.compile(
        "(?i)\\b(password|passwd|pwd|token|secret|authorization|api[_-]?key|private[_ -]?key)"
            + "(\\s*[:=]\\s*)([^\\s,;]+)");
    private static final Pattern URL_CREDENTIALS = Pattern.compile(
        "(?i)([a-z][a-z0-9+.-]*://[^:/\\s]+:)([^@/\\s]+)(@)");

    private final ObjectMapper objectMapper;
    private final TaskEngineProperties properties;

    public DiagnosticResultSanitizer(ObjectMapper objectMapper, TaskEngineProperties properties) {
        this.objectMapper = objectMapper;
        this.properties = properties;
    }

    public SanitizedDiagnostic sanitize(JsonNode audit) {
        RedactedText stdout = sanitizeText(text(audit, "stdoutText", "stdout"));
        RedactedText stderr = sanitizeText(text(audit, "stderrText", "stderr"));
        boolean wasRedacted = audit.path("redacted").asBoolean(false);
        boolean wasTruncated = audit.path("truncated").asBoolean(false);
        boolean redactedFlag = wasRedacted || stdout.redacted() || stderr.redacted();
        boolean truncatedFlag = wasTruncated || stdout.truncated() || stderr.truncated();
        ObjectNode result = objectMapper.createObjectNode();
        if (audit.hasNonNull("exitCode")) result.put("exitCode", audit.path("exitCode").asInt());
        for (String field : new String[] {"reason", "command", "arg", "operator", "replyKind"}) {
            if (audit.hasNonNull(field)) result.put(field, audit.path(field).asText());
        }
        if (audit.hasNonNull("signal")) result.put("signal", audit.path("signal").asInt());
        for (String field : new String[] {"timedOut", "cleanupConfirmed"}) {
            if (audit.hasNonNull(field)) result.put(field, audit.path(field).asBoolean());
        }
        result.put("stdout", stdout.value());
        result.put("stderr", stderr.value());
        if (audit.hasNonNull("requestTs")) result.put("requestTs", audit.path("requestTs").asLong());
        if (audit.hasNonNull("replyTs")) result.put("replyTs", audit.path("replyTs").asLong());
        else if (audit.hasNonNull("ts")) result.put("replyTs", audit.path("ts").asLong());
        result.put("redacted", redactedFlag);
        result.put("truncated", truncatedFlag);
        try {
            return new SanitizedDiagnostic(objectMapper.writeValueAsString(result),
                redactedFlag, truncatedFlag);
        } catch (JsonProcessingException exception) {
            throw new IllegalStateException("诊断结果序列化失败", exception);
        }
    }

    private String text(JsonNode node, String preferred, String fallback) {
        if (node != null && node.hasNonNull(preferred)) return node.path(preferred).asText("");
        return node == null ? "" : node.path(fallback).asText("");
    }

    private RedactedText sanitizeText(String raw) {
        String value = raw == null ? "" : raw;
        String redacted = PRIVATE_KEY.matcher(value).replaceAll("[REDACTED_PRIVATE_KEY]");
        redacted = BEARER_TOKEN.matcher(redacted).replaceAll("Bearer [REDACTED]");
        redacted = replaceAssignments(redacted);
        redacted = URL_CREDENTIALS.matcher(redacted).replaceAll("$1[REDACTED]$3");
        boolean changed = !redacted.equals(value);
        int maxChars = properties.remoteDiagnostic().maxOutputChars();
        boolean truncated = redacted.length() > maxChars;
        if (truncated) redacted = redacted.substring(0, maxChars) + "\n[OUTPUT_TRUNCATED]";
        return new RedactedText(redacted, changed, truncated);
    }

    private String replaceAssignments(String value) {
        Matcher matcher = SECRET_ASSIGNMENT.matcher(value);
        StringBuffer result = new StringBuffer();
        while (matcher.find()) {
            matcher.appendReplacement(result, Matcher.quoteReplacement(
                matcher.group(1) + matcher.group(2) + "[REDACTED]"));
        }
        matcher.appendTail(result);
        return result.toString();
    }

    public record SanitizedDiagnostic(String resultJson, boolean redacted, boolean truncated) {
    }

    private record RedactedText(String value, boolean redacted, boolean truncated) {
    }
}
