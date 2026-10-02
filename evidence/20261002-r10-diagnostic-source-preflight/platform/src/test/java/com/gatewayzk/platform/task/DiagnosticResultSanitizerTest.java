package com.gatewayzk.platform.task;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import org.junit.jupiter.api.Test;

import java.time.Duration;
import java.util.List;

import static org.assertj.core.api.Assertions.assertThat;

class DiagnosticResultSanitizerTest {

    private final ObjectMapper objectMapper = new ObjectMapper();

    @Test
    void redactsCommonSecretShapesWithoutPersistingRawCredentials() throws Exception {
        DiagnosticResultSanitizer sanitizer = sanitizer(10_000);
        JsonNode audit = objectMapper.readTree("""
            {"success":true,"exitCode":0,
             "stdoutText":"token=raw-token password:raw-password https://user:raw-pass@example.test",
             "stderrText":"Authorization: Bearer abc.def.ghi"}
            """);

        DiagnosticResultSanitizer.SanitizedDiagnostic result = sanitizer.sanitize(audit);

        assertThat(result.redacted()).isTrue();
        assertThat(result.resultJson()).doesNotContain(
            "raw-token", "raw-password", "raw-pass", "abc.def.ghi");
        assertThat(result.resultJson()).contains("[REDACTED]");
    }

    @Test
    void capsEachOutputStreamAndMarksTheStoredResult() throws Exception {
        DiagnosticResultSanitizer sanitizer = sanitizer(1_000);
        JsonNode audit = objectMapper.createObjectNode().put("stdoutText", "x".repeat(1_200));

        DiagnosticResultSanitizer.SanitizedDiagnostic result = sanitizer.sanitize(audit);

        assertThat(result.truncated()).isTrue();
        assertThat(result.resultJson()).contains("OUTPUT_TRUNCATED", "\"truncated\":true");
    }

    @Test
    void redactsIncompleteFakePrivateKeyBeforeOutputTruncation() throws Exception {
        DiagnosticResultSanitizer sanitizer = sanitizer(1_000);
        String fake = "-----BEGIN PRIVATE KEY-----\nFAKE_KEY_MATERIAL_123\n" + "x".repeat(131_100);
        var audit = objectMapper.createObjectNode().put("stdout", fake).put("truncated", true);
        var result = sanitizer.sanitize(audit);
        assertThat(result.resultJson()).doesNotContain("FAKE_KEY_MATERIAL_123", "BEGIN PRIVATE KEY");
        assertThat(result.resultJson()).contains("[REDACTED_PRIVATE_KEY]", "\"truncated\":true");
    }

    private DiagnosticResultSanitizer sanitizer(int maxChars) {
        TaskEngineProperties properties = new TaskEngineProperties(false, 2_000, 20,
            new TaskEngineProperties.RemoteDiagnostic(Duration.ofMinutes(2), maxChars,
                List.of("uptime")));
        return new DiagnosticResultSanitizer(objectMapper, properties);
    }
}
