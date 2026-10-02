package com.gatewayzk.platform.edge;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ArrayNode;
import org.springframework.dao.DuplicateKeyException;
import org.springframework.beans.factory.ObjectProvider;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.jdbc.core.namedparam.MapSqlParameterSource;
import org.springframework.jdbc.core.namedparam.NamedParameterJdbcTemplate;
import org.springframework.stereotype.Repository;
import org.springframework.transaction.annotation.Isolation;
import org.springframework.transaction.annotation.Propagation;
import org.springframework.transaction.annotation.Transactional;

import java.sql.Timestamp;
import java.sql.ResultSet;
import java.sql.SQLException;
import java.time.Instant;
import java.util.List;
import java.util.Optional;

@Repository
public class EdgeRuntimeRepository {

    private static final int POINT_UPSERT_CHUNK_SIZE = 250;
    // Protective lease budget, including replacement overlap; not a hardware/user capacity claim.
    private static final int MAX_UNEXPIRED_REALTIME_LEASES_PER_DEVICE = 8;

    private final NamedParameterJdbcTemplate jdbc;
    private final ObjectMapper objectMapper;
    private final TimescalePointStore timescale;
    private final boolean postgresql;

    public EdgeRuntimeRepository(NamedParameterJdbcTemplate jdbc, ObjectMapper objectMapper) {
        this(jdbc, objectMapper, null, "mysql");
    }

    public EdgeRuntimeRepository(NamedParameterJdbcTemplate jdbc, ObjectMapper objectMapper,
                                 ObjectProvider<TimescalePointStore> timescale) {
        this(jdbc, objectMapper, timescale, "mysql");
    }

    @Autowired
    public EdgeRuntimeRepository(NamedParameterJdbcTemplate jdbc, ObjectMapper objectMapper,
                                 ObjectProvider<TimescalePointStore> timescale,
                                 @Value("${platform.database.dialect:mysql}") String dialect) {
        this.jdbc = jdbc;
        this.objectMapper = objectMapper;
        this.timescale = timescale == null ? null : timescale.getIfAvailable();
        this.postgresql = "postgresql".equals(dialect);
    }

    public Optional<Long> findDeviceId(String machineCode) {
        return jdbc.query("SELECT id FROM device_asset WHERE machine_code=:machineCode AND status <> 'ARCHIVED'",
            new MapSqlParameterSource("machineCode", machineCode),
            (rs, rowNum) -> rs.getLong("id")).stream().findFirst();
    }

    @Transactional(propagation = Propagation.REQUIRES_NEW, isolation = Isolation.READ_COMMITTED)
    public void createRealtimeLease(String sessionId, long deviceId, String meterCode,
                                    int intervalMs, Instant expiresAt, Long ownerUserId) {
        var params = new MapSqlParameterSource("deviceId", deviceId);
        // Serialize all instances on the existing device row, and commit before MQTT publication.
        jdbc.queryForObject("SELECT id FROM device_asset WHERE id=:deviceId FOR UPDATE",
            params, Long.class);
        // STOPPING/STOP_FAILED still occupy the edge until expiry or confirmed stop deletion.
        long unexpiredLeases = jdbc.queryForObject("""
            SELECT COUNT(*) FROM edge_realtime_lease
            WHERE device_id=:deviceId AND expires_at > :now AND lease_state <> 'REJECTED'
            """, params.addValue("now", Timestamp.from(Instant.now())), Long.class);
        if (unexpiredLeases >= MAX_UNEXPIRED_REALTIME_LEASES_PER_DEVICE) {
            throw new RealtimeLeaseLimitExceededException();
        }
        jdbc.update("""
            INSERT INTO edge_realtime_lease(
                session_id,device_id,meter_code,interval_ms,expires_at,owner_user_id
            ) VALUES (
                :sessionId,:deviceId,:meterCode,:intervalMs,:expiresAt,:ownerUserId
            )
            """, new MapSqlParameterSource("sessionId", sessionId)
            .addValue("deviceId", deviceId).addValue("meterCode", meterCode)
            .addValue("intervalMs", intervalMs)
            .addValue("ownerUserId", ownerUserId)
            .addValue("expiresAt", Timestamp.from(expiresAt)));
    }

    public boolean realtimeLeaseOwnedBy(String sessionId, long deviceId, long userId) {
        return jdbc.queryForObject("""
            SELECT COUNT(*) FROM edge_realtime_lease
            WHERE session_id=:sessionId AND device_id=:deviceId AND owner_user_id=:userId
            """, new MapSqlParameterSource("sessionId", sessionId)
            .addValue("deviceId", deviceId).addValue("userId", userId), Long.class) == 1;
    }

    @Transactional(propagation = Propagation.REQUIRES_NEW)
    public void createSystemMonitorLease(String sessionId, long deviceId, long ownerUserId, Instant expiresAt) {
        jdbc.update("""
            INSERT INTO edge_system_monitor_lease(session_id,device_id,owner_user_id,expires_at)
            VALUES(:sessionId,:deviceId,:ownerUserId,:expiresAt)
            """, new MapSqlParameterSource("sessionId", sessionId).addValue("deviceId", deviceId)
            .addValue("ownerUserId", ownerUserId).addValue("expiresAt", Timestamp.from(expiresAt)));
    }

    public boolean systemMonitorLeaseOwnedBy(String sessionId, long deviceId, long userId) {
        return jdbc.queryForObject("""
            SELECT COUNT(*) FROM edge_system_monitor_lease
            WHERE session_id=:sessionId AND device_id=:deviceId AND owner_user_id=:userId
            """, new MapSqlParameterSource("sessionId", sessionId)
            .addValue("deviceId", deviceId).addValue("userId", userId), Long.class) == 1;
    }

    public void deleteSystemMonitorLease(String sessionId, long deviceId) {
        jdbc.update("DELETE FROM edge_system_monitor_lease WHERE session_id=:sessionId AND device_id=:deviceId",
            new MapSqlParameterSource("sessionId", sessionId).addValue("deviceId", deviceId));
    }

    public void deleteSystemMonitorLeasesExpiredBefore(Instant cutoff) {
        jdbc.update("DELETE FROM edge_system_monitor_lease WHERE expires_at < :cutoff",
            new MapSqlParameterSource("cutoff", Timestamp.from(cutoff)));
    }

    public int markRealtimeLeaseActive(String sessionId, long deviceId) {
        return jdbc.update("""
            UPDATE edge_realtime_lease
            SET lease_state='ACTIVE', updated_at=CURRENT_TIMESTAMP
            WHERE session_id=:sessionId AND device_id=:deviceId AND lease_state='STARTING'
              AND expires_at > :now
            """, new MapSqlParameterSource("sessionId", sessionId)
            .addValue("deviceId", deviceId).addValue("now", Timestamp.from(Instant.now())));
    }

    public int rejectRealtimeLease(String sessionId, long deviceId, String reasonCode) {
        return jdbc.update("""
            UPDATE edge_realtime_lease SET lease_state='REJECTED', reason_code=:reasonCode,
                updated_at=CURRENT_TIMESTAMP
            WHERE session_id=:sessionId AND device_id=:deviceId AND lease_state='STARTING'
              AND expires_at > :now
            """, new MapSqlParameterSource("sessionId", sessionId).addValue("deviceId", deviceId)
            .addValue("reasonCode", reasonCode).addValue("now", Timestamp.from(Instant.now())));
    }

    public Optional<RealtimeLease> realtimeLease(String sessionId) {
        return jdbc.query("""
            SELECT l.session_id,l.device_id,d.machine_code,l.meter_code,l.expires_at,
                   l.lease_state,l.updated_at,l.reason_code
            FROM edge_realtime_lease l
            JOIN device_asset d ON d.id=l.device_id
            WHERE l.session_id=:sessionId AND d.status <> 'ARCHIVED'
            """, new MapSqlParameterSource("sessionId", sessionId),
            (rs, rowNum) -> new RealtimeLease(
                rs.getString("session_id"), rs.getLong("device_id"),
                rs.getString("machine_code"), rs.getString("meter_code"),
                rs.getTimestamp("expires_at").toInstant(),
                rs.getString("lease_state"), rs.getTimestamp("updated_at").toInstant(), rs.getString("reason_code")
            )).stream().findFirst();
    }

    public Optional<RealtimeLease> activeRealtimeLease(String sessionId, long deviceId,
                                                        Instant now) {
        return activeRealtimeLease(sessionId, deviceId, now, false);
    }

    public Optional<RealtimeLease> activeRealtimeLeaseForUpdate(String sessionId, long deviceId,
                                                                 Instant now) {
        return activeRealtimeLease(sessionId, deviceId, now, true);
    }

    private Optional<RealtimeLease> activeRealtimeLease(String sessionId, long deviceId,
                                                        Instant now, boolean forUpdate) {
        String sql = """
            SELECT l.session_id,l.device_id,d.machine_code,l.meter_code,l.expires_at,
                   l.lease_state,l.updated_at,l.reason_code
            FROM edge_realtime_lease l
            JOIN device_asset d ON d.id=l.device_id
            WHERE l.session_id=:sessionId AND l.device_id=:deviceId
              AND l.owner_user_id IS NOT NULL
              AND l.lease_state IN ('STARTING','ACTIVE') AND l.expires_at > :now
              AND d.status <> 'ARCHIVED'
            """ + (forUpdate ? (postgresql ? " FOR UPDATE OF l" : " FOR UPDATE") : "");
        return jdbc.query(sql, new MapSqlParameterSource("sessionId", sessionId)
            .addValue("deviceId", deviceId).addValue("now", Timestamp.from(now)),
            (rs, rowNum) -> new RealtimeLease(
                rs.getString("session_id"), rs.getLong("device_id"),
                rs.getString("machine_code"), rs.getString("meter_code"),
                rs.getTimestamp("expires_at").toInstant(),
                rs.getString("lease_state"), rs.getTimestamp("updated_at").toInstant(), rs.getString("reason_code")
            )).stream().findFirst();
    }

    public int claimRealtimeLeaseForStop(String sessionId, long deviceId, String claimToken) {
        return jdbc.update("""
            UPDATE edge_realtime_lease
            SET lease_state='STOPPING', stop_claim_token=:claimToken,
                updated_at=CURRENT_TIMESTAMP
            WHERE session_id=:sessionId AND device_id=:deviceId
              AND lease_state IN ('STARTING','ACTIVE','STOP_FAILED')
            """, new MapSqlParameterSource("sessionId", sessionId)
            .addValue("deviceId", deviceId)
            .addValue("claimToken", claimToken));
    }

    public int markRealtimeLeaseStopFailed(String sessionId, long deviceId, String claimToken) {
        return jdbc.update("""
            UPDATE edge_realtime_lease
            SET lease_state='STOP_FAILED', stop_claim_token=NULL,
                updated_at=CURRENT_TIMESTAMP
            WHERE session_id=:sessionId AND device_id=:deviceId
              AND lease_state='STOPPING' AND stop_claim_token=:claimToken
            """, new MapSqlParameterSource("sessionId", sessionId)
            .addValue("deviceId", deviceId)
            .addValue("claimToken", claimToken));
    }

    public int deleteRealtimeLease(String sessionId, long deviceId) {
        return jdbc.update("""
            DELETE FROM edge_realtime_lease
            WHERE session_id=:sessionId AND device_id=:deviceId
            """, new MapSqlParameterSource("sessionId", sessionId)
            .addValue("deviceId", deviceId));
    }

    public int deleteStoppingRealtimeLease(String sessionId, long deviceId, String claimToken) {
        return jdbc.update("""
            DELETE FROM edge_realtime_lease
            WHERE session_id=:sessionId AND device_id=:deviceId
              AND lease_state='STOPPING' AND stop_claim_token=:claimToken
            """, new MapSqlParameterSource("sessionId", sessionId)
            .addValue("deviceId", deviceId)
            .addValue("claimToken", claimToken));
    }

    public int deleteRealtimeLeasesExpiredBefore(Instant cutoff) {
        return jdbc.update("""
            DELETE FROM edge_realtime_lease
            WHERE expires_at < :cutoff
              AND (lease_state <> 'STOPPING' OR updated_at < :cutoff)
            """,
            new MapSqlParameterSource("cutoff", Timestamp.from(cutoff)));
    }

    long realtimeLeaseCount(long deviceId) {
        Long count = jdbc.queryForObject(
            "SELECT COUNT(*) FROM edge_realtime_lease WHERE device_id=:deviceId",
            new MapSqlParameterSource("deviceId", deviceId), Long.class);
        return count == null ? 0L : count;
    }

    public void recordSeen(long deviceId, Instant sourceTime) {
        jdbc.update("""
            UPDATE device_asset SET connection_status='ONLINE', last_seen_at=:seenAt,
                updated_at=CURRENT_TIMESTAMP
            WHERE id=:deviceId AND (last_seen_at IS NULL OR last_seen_at <= :seenAt)
            """, new MapSqlParameterSource("deviceId", deviceId)
            .addValue("seenAt", Timestamp.from(sourceTime)));
    }

    public int markOfflineBefore(Instant cutoff) {
        return jdbc.update("""
            UPDATE device_asset SET connection_status='OFFLINE', updated_at=CURRENT_TIMESTAMP
            WHERE status <> 'ARCHIVED' AND connection_status='ONLINE'
              AND last_seen_at IS NOT NULL AND last_seen_at < :cutoff
            """, new MapSqlParameterSource("cutoff", Timestamp.from(cutoff)));
    }

    public void upsertPoint(PointValue point) {
        if (timescale != null || postgresql) {
            upsertPoints(List.of(point));
            return;
        }
        MapSqlParameterSource params = point.params();
        int changed = jdbc.update("""
            UPDATE edge_realtime_point SET point_code=:pointCode, point_name=:pointName,
                value_json=:valueJson, quality=:quality, source_ts=:sourceTs,
                received_at=:receivedAt, stale=:stale, alarm_active=:alarmActive
            WHERE device_id=:deviceId AND meter_code=:meterCode AND point_index=:pointIndex
              AND source_ts <= :sourceTs
            """, params);
        if (changed > 0) return;
        try {
            jdbc.update("""
                INSERT INTO edge_realtime_point(device_id,meter_code,point_index,point_code,
                    point_name,value_json,quality,source_ts,received_at,stale,alarm_active)
                VALUES (:deviceId,:meterCode,:pointIndex,:pointCode,:pointName,:valueJson,
                    :quality,:sourceTs,:receivedAt,:stale,:alarmActive)
                """, params);
        } catch (DuplicateKeyException ignored) {
            // A concurrent or newer message already established this point.
        }
    }

    public void upsertPoints(List<PointValue> points) {
        if (points == null || points.isEmpty()) return;
        if (timescale != null) {
            long deviceId = points.getFirst().deviceId();
            if (points.stream().anyMatch(point -> point.deviceId() != deviceId)) {
                throw new IllegalArgumentException("A telemetry snapshot must belong to one device");
            }
            timescale.upsert(machineCode(deviceId), points);
            return;
        }
        if (postgresql) {
            record PointKey(long device, String meter, int index) {}
            var unique = new java.util.LinkedHashMap<PointKey, PointValue>();
            for (var point : points) {
                unique.merge(new PointKey(point.deviceId(), point.meterCode(), point.pointIndex()), point,
                    (previous, incoming) -> incoming.sourceTs() >= previous.sourceTs() ? incoming : previous);
            }
            points = List.copyOf(unique.values());
        }
        for (int offset = 0; offset < points.size(); offset += POINT_UPSERT_CHUNK_SIZE) {
            int end = Math.min(offset + POINT_UPSERT_CHUNK_SIZE, points.size());
            upsertPointChunk(points.subList(offset, end));
        }
    }

    public void upsertSessionPoints(String sessionId, List<PointValue> points) {
        if (points == null || points.isEmpty()) return;
        if (postgresql) {
            record PointKey(long device, String meter, int index) { }
            var unique = new java.util.LinkedHashMap<PointKey, PointValue>();
            for (var point : points) {
                unique.merge(new PointKey(point.deviceId(), point.meterCode(), point.pointIndex()), point,
                    (old, incoming) -> incoming.sourceTs() >= old.sourceTs() ? incoming : old);
            }
            points = List.copyOf(unique.values());
        }
        for (int offset = 0; offset < points.size(); offset += POINT_UPSERT_CHUNK_SIZE) {
            int end = Math.min(offset + POINT_UPSERT_CHUNK_SIZE, points.size());
            upsertSessionPointChunk(sessionId, points.subList(offset, end));
        }
    }

    private void upsertSessionPointChunk(String sessionId, List<PointValue> points) {
        StringBuilder sql = new StringBuilder("""
            INSERT INTO edge_realtime_session_point(session_id,device_id,meter_code,point_index,
                point_code,point_name,value_json,quality,source_ts,received_at,stale,alarm_active)
            VALUES
            """);
        MapSqlParameterSource params = new MapSqlParameterSource("sessionId", sessionId);
        for (int index = 0; index < points.size(); index++) {
            if (index > 0) sql.append(',');
            String suffix = Integer.toString(index);
            sql.append("(:sessionId,:deviceId").append(suffix)
                .append(" ,:meterCode").append(suffix)
                .append(" ,:pointIndex").append(suffix)
                .append(" ,:pointCode").append(suffix)
                .append(" ,:pointName").append(suffix)
                .append(" ,:valueJson").append(suffix)
                .append(" ,:quality").append(suffix)
                .append(" ,:sourceTs").append(suffix)
                .append(" ,:receivedAt").append(suffix)
                .append(" ,:stale").append(suffix)
                .append(" ,:alarmActive").append(suffix).append(')');
            points.get(index).addParams(params, suffix);
        }
        if (postgresql) {
            sql.append("""
                ON CONFLICT (session_id,meter_code,point_index) DO UPDATE SET
                    point_code=EXCLUDED.point_code,point_name=EXCLUDED.point_name,
                    value_json=EXCLUDED.value_json,quality=EXCLUDED.quality,
                    received_at=EXCLUDED.received_at,stale=EXCLUDED.stale,
                    alarm_active=EXCLUDED.alarm_active,source_ts=EXCLUDED.source_ts
                WHERE edge_realtime_session_point.source_ts <= EXCLUDED.source_ts
                """);
            jdbc.update(sql.toString(), params);
            return;
        }
        sql.append("""

            ON DUPLICATE KEY UPDATE
                point_code=CASE WHEN edge_realtime_session_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(point_code) ELSE edge_realtime_session_point.point_code END,
                point_name=CASE WHEN edge_realtime_session_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(point_name) ELSE edge_realtime_session_point.point_name END,
                value_json=CASE WHEN edge_realtime_session_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(value_json) ELSE edge_realtime_session_point.value_json END,
                quality=CASE WHEN edge_realtime_session_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(quality) ELSE edge_realtime_session_point.quality END,
                received_at=CASE WHEN edge_realtime_session_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(received_at) ELSE edge_realtime_session_point.received_at END,
                stale=CASE WHEN edge_realtime_session_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(stale) ELSE edge_realtime_session_point.stale END,
                alarm_active=CASE WHEN edge_realtime_session_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(alarm_active) ELSE edge_realtime_session_point.alarm_active END,
                source_ts=GREATEST(edge_realtime_session_point.source_ts, VALUES(source_ts))
            """);
        jdbc.update(sql.toString(), params);
    }

    private void upsertPointChunk(List<PointValue> points) {
        StringBuilder sql = new StringBuilder("""
            INSERT INTO edge_realtime_point(device_id,meter_code,point_index,point_code,
                point_name,value_json,quality,source_ts,received_at,stale,alarm_active)
            VALUES
            """);
        MapSqlParameterSource params = new MapSqlParameterSource();
        for (int index = 0; index < points.size(); index++) {
            if (index > 0) sql.append(',');
            String suffix = Integer.toString(index);
            sql.append("(:deviceId").append(suffix)
                .append(",:meterCode").append(suffix)
                .append(",:pointIndex").append(suffix)
                .append(",:pointCode").append(suffix)
                .append(",:pointName").append(suffix)
                .append(",:valueJson").append(suffix)
                .append(",:quality").append(suffix)
                .append(",:sourceTs").append(suffix)
                .append(",:receivedAt").append(suffix)
                .append(",:stale").append(suffix)
                .append(",:alarmActive").append(suffix).append(')');
            points.get(index).addParams(params, suffix);
        }
        if (postgresql) {
            sql.append("""
                ON CONFLICT (device_id,meter_code,point_index) DO UPDATE SET
                    point_code=EXCLUDED.point_code,point_name=EXCLUDED.point_name,
                    value_json=EXCLUDED.value_json,quality=EXCLUDED.quality,
                    received_at=EXCLUDED.received_at,stale=EXCLUDED.stale,
                    alarm_active=EXCLUDED.alarm_active,source_ts=EXCLUDED.source_ts
                WHERE edge_realtime_point.source_ts <= EXCLUDED.source_ts
                """);
            jdbc.update(sql.toString(), params);
            return;
        }
        sql.append("""

            ON DUPLICATE KEY UPDATE
                point_code=CASE WHEN edge_realtime_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(point_code) ELSE edge_realtime_point.point_code END,
                point_name=CASE WHEN edge_realtime_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(point_name) ELSE edge_realtime_point.point_name END,
                value_json=CASE WHEN edge_realtime_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(value_json) ELSE edge_realtime_point.value_json END,
                quality=CASE WHEN edge_realtime_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(quality) ELSE edge_realtime_point.quality END,
                received_at=CASE WHEN edge_realtime_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(received_at) ELSE edge_realtime_point.received_at END,
                stale=CASE WHEN edge_realtime_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(stale) ELSE edge_realtime_point.stale END,
                alarm_active=CASE WHEN edge_realtime_point.source_ts <= VALUES(source_ts)
                    THEN VALUES(alarm_active) ELSE edge_realtime_point.alarm_active END,
                source_ts=GREATEST(edge_realtime_point.source_ts, VALUES(source_ts))
            """);
        jdbc.update(sql.toString(), params);
    }

    public ArrayNode realtimePoints(long deviceId) {
        ArrayNode result = objectMapper.createArrayNode();
        if (timescale != null) {
            for (var point : timescale.latest(machineCode(deviceId), deviceId)) {
                var item = result.addObject();
                item.put("meterCode", point.meterCode());
                item.put("index", point.pointIndex());
                putText(item, "pointCode", point.pointCode());
                putText(item, "name", point.pointName());
                if (point.valueJson() == null) item.putNull("value");
                else item.set("value", json(point.valueJson()));
                item.put("quality", point.quality());
                item.put("ts", point.sourceTs());
                item.put("receivedAt", point.receivedAt().toEpochMilli());
                item.put("stale", point.stale());
                item.put("alarmActive", point.alarmActive());
            }
            return result;
        }
        jdbc.query("""
            SELECT meter_code,point_index,point_code,point_name,value_json,quality,
                   source_ts,received_at,stale,alarm_active
            FROM edge_realtime_point WHERE device_id=:deviceId
            ORDER BY meter_code,point_index
            """, new MapSqlParameterSource("deviceId", deviceId), rs -> {
                appendPoint(result, rs);
            });
        return result;
    }

    private void appendPoint(ArrayNode result, ResultSet rs) throws SQLException {
        var item = result.addObject();
        item.put("meterCode", rs.getString("meter_code"));
        item.put("index", rs.getInt("point_index"));
        putText(item, "pointCode", rs.getString("point_code"));
        putText(item, "name", rs.getString("point_name"));
        String valueJson = rs.getString("value_json");
        if (valueJson == null) item.putNull("value");
        else item.set("value", json(valueJson));
        int quality = rs.getInt("quality");
        if (rs.wasNull()) item.putNull("quality"); else item.put("quality", quality);
        item.put("ts", rs.getLong("source_ts"));
        item.put("receivedAt", rs.getTimestamp("received_at").toInstant().toEpochMilli());
        item.put("stale", rs.getBoolean("stale"));
        boolean alarm = rs.getBoolean("alarm_active");
        if (rs.wasNull()) item.putNull("alarmActive"); else item.put("alarmActive", alarm);
    }

    public ArrayNode realtimeSessionPoints(String sessionId, long deviceId, Instant now) {
        ArrayNode result = objectMapper.createArrayNode();
        jdbc.query("""
            SELECT p.meter_code,p.point_index,p.point_code,p.point_name,p.value_json,p.quality,
                   p.source_ts,p.received_at,p.stale,p.alarm_active
            FROM edge_realtime_session_point p
            JOIN edge_realtime_lease l ON l.session_id=p.session_id
            WHERE p.session_id=:sessionId AND p.device_id=:deviceId
              AND l.device_id=:deviceId AND l.lease_state IN ('STARTING','ACTIVE')
              AND l.expires_at > :now
            ORDER BY p.meter_code,p.point_index
            """, new MapSqlParameterSource("sessionId", sessionId)
            .addValue("deviceId", deviceId).addValue("now", Timestamp.from(now)), rs -> {
                appendPoint(result, rs);
            });
        return result;
    }

    public void upsertConfigSnapshot(long deviceId, String requestId, String payloadJson,
                                     long sourceTs, Instant receivedAt) {
        MapSqlParameterSource params = new MapSqlParameterSource("deviceId", deviceId)
            .addValue("requestId", requestId).addValue("payloadJson", payloadJson)
            .addValue("sourceTs", sourceTs).addValue("receivedAt", Timestamp.from(receivedAt));
        if (postgresql) {
            // Only device identity is an expected conflict; another device's request ID is an error.
            jdbc.update("""
                INSERT INTO edge_config_snapshot(device_id,request_id,payload_json,source_ts,received_at)
                VALUES (:deviceId,:requestId,:payloadJson,:sourceTs,:receivedAt)
                ON CONFLICT (device_id) DO UPDATE SET request_id=EXCLUDED.request_id,
                    payload_json=EXCLUDED.payload_json,source_ts=EXCLUDED.source_ts,
                    received_at=EXCLUDED.received_at
                WHERE edge_config_snapshot.source_ts <= EXCLUDED.source_ts
                """, params);
            return;
        }
        int changed = jdbc.update("""
            UPDATE edge_config_snapshot SET request_id=:requestId,payload_json=:payloadJson,
                source_ts=:sourceTs,received_at=:receivedAt
            WHERE device_id=:deviceId AND source_ts <= :sourceTs
            """, params);
        if (changed > 0) return;
        try {
            jdbc.update("""
                INSERT INTO edge_config_snapshot(device_id,request_id,payload_json,source_ts,received_at)
                VALUES (:deviceId,:requestId,:payloadJson,:sourceTs,:receivedAt)
                """, params);
        } catch (DuplicateKeyException ignored) {
            // Duplicate request or a newer snapshot already won the race.
        }
    }

    public Optional<JsonNode> configSnapshot(long deviceId) {
        return jdbc.query("SELECT payload_json,received_at FROM edge_config_snapshot WHERE device_id=:deviceId",
            new MapSqlParameterSource("deviceId", deviceId), (rs, rowNum) -> {
                JsonNode stored = json(rs.getString("payload_json"));
                var result = stored.isObject() ? stored.deepCopy() : objectMapper.createObjectNode();
                ((com.fasterxml.jackson.databind.node.ObjectNode) result).put("source", "NATIVE_MQTT");
                ((com.fasterxml.jackson.databind.node.ObjectNode) result).put("receivedAt",
                    rs.getTimestamp("received_at").toInstant().toEpochMilli());
                return result;
            }).stream().findFirst();
    }

    public void upsertSystemTelemetry(long deviceId, String payloadJson, long sourceTs,
                                      Instant receivedAt) {
        MapSqlParameterSource params = new MapSqlParameterSource("deviceId", deviceId)
            .addValue("payloadJson", payloadJson).addValue("sourceTs", sourceTs)
            .addValue("receivedAt", Timestamp.from(receivedAt));
        if (postgresql) {
            jdbc.update("""
                INSERT INTO edge_system_snapshot(device_id,payload_json,source_ts,received_at)
                VALUES (:deviceId,:payloadJson,:sourceTs,:receivedAt)
                ON CONFLICT (device_id) DO UPDATE SET payload_json=EXCLUDED.payload_json,
                    source_ts=EXCLUDED.source_ts,received_at=EXCLUDED.received_at
                WHERE edge_system_snapshot.source_ts <= EXCLUDED.source_ts
                """, params);
            return;
        }
        int changed = jdbc.update("""
            UPDATE edge_system_snapshot SET payload_json=:payloadJson,source_ts=:sourceTs,
                received_at=:receivedAt
            WHERE device_id=:deviceId AND source_ts <= :sourceTs
            """, params);
        if (changed > 0) return;
        try {
            jdbc.update("""
                INSERT INTO edge_system_snapshot(device_id,payload_json,source_ts,received_at)
                VALUES (:deviceId,:payloadJson,:sourceTs,:receivedAt)
                """, params);
        } catch (DuplicateKeyException ignored) {
            // A concurrent or newer telemetry sample already won the race.
        }
    }

    public Optional<JsonNode> systemTelemetry(long deviceId) {
        return jdbc.query("SELECT payload_json,received_at FROM edge_system_snapshot WHERE device_id=:deviceId",
            new MapSqlParameterSource("deviceId", deviceId), (rs, rowNum) -> {
                JsonNode stored = json(rs.getString("payload_json"));
                var result = stored.isObject() ? stored.deepCopy() : objectMapper.createObjectNode();
                ((com.fasterxml.jackson.databind.node.ObjectNode) result).put("source", "NATIVE_MQTT");
                ((com.fasterxml.jackson.databind.node.ObjectNode) result).put("receivedAt",
                    rs.getTimestamp("received_at").toInstant().toEpochMilli());
                return result;
            }).stream().findFirst();
    }

    public void upsertCameraStatus(long deviceId, String cameraCode, String payloadJson,
                                   long sourceTs, Instant receivedAt) {
        MapSqlParameterSource params = new MapSqlParameterSource("deviceId", deviceId)
            .addValue("cameraCode", cameraCode).addValue("payloadJson", payloadJson)
            .addValue("sourceTs", sourceTs).addValue("receivedAt", Timestamp.from(receivedAt));
        if (postgresql) {
            jdbc.update("""
                INSERT INTO edge_camera_status(device_id,camera_code,payload_json,source_ts,received_at)
                VALUES (:deviceId,:cameraCode,:payloadJson,:sourceTs,:receivedAt)
                ON CONFLICT (device_id,camera_code) DO UPDATE SET payload_json=EXCLUDED.payload_json,
                    source_ts=EXCLUDED.source_ts,received_at=EXCLUDED.received_at
                WHERE edge_camera_status.source_ts <= EXCLUDED.source_ts
                """, params);
            return;
        }
        int changed = jdbc.update("""
            UPDATE edge_camera_status SET payload_json=:payloadJson,source_ts=:sourceTs,
                received_at=:receivedAt
            WHERE device_id=:deviceId AND camera_code=:cameraCode AND source_ts <= :sourceTs
            """, params);
        if (changed > 0) return;
        try {
            jdbc.update("""
                INSERT INTO edge_camera_status(device_id,camera_code,payload_json,source_ts,received_at)
                VALUES (:deviceId,:cameraCode,:payloadJson,:sourceTs,:receivedAt)
                """, params);
        } catch (DuplicateKeyException ignored) {
            // A concurrent or newer status already won the race.
        }
    }

    public ArrayNode cameraStatuses(long deviceId) {
        ArrayNode result = objectMapper.createArrayNode();
        jdbc.query("""
            SELECT payload_json,received_at FROM edge_camera_status
            WHERE device_id=:deviceId ORDER BY camera_code
            """, new MapSqlParameterSource("deviceId", deviceId), rs -> {
            JsonNode stored = json(rs.getString("payload_json"));
            var item = stored.isObject() ? stored.deepCopy() : objectMapper.createObjectNode();
            ((com.fasterxml.jackson.databind.node.ObjectNode) item).put("receivedAt",
                rs.getTimestamp("received_at").toInstant().toEpochMilli());
            result.add(item);
        });
        return result;
    }

    public void insertDiagnosticResult(long deviceId, String commandId, String payloadJson,
                                       Instant receivedAt) {
        try {
            jdbc.update("""
                INSERT INTO edge_diagnostic_reply(cmd_id,device_id,payload_json,received_at)
                VALUES (:commandId,:deviceId,:payloadJson,:receivedAt)
                """ + (postgresql ? " ON CONFLICT (cmd_id) DO NOTHING" : ""),
                new MapSqlParameterSource("commandId", commandId)
                .addValue("deviceId", deviceId).addValue("payloadJson", payloadJson)
                .addValue("receivedAt", Timestamp.from(receivedAt)));
        } catch (DuplicateKeyException ignored) {
            // QoS retransmission of a terminal diagnostic reply is idempotent.
            if (postgresql) throw ignored;
        }
    }

    @Transactional(propagation = Propagation.REQUIRES_NEW)
    public void reserveDiagnosticRequest(String commandId, long deviceId, String command,
                                         String argument, String operator, long requestTs) {
        jdbc.update("""
            INSERT INTO edge_diagnostic_request
                (cmd_id,device_id,command_name,argument,operator_name,request_ts)
            VALUES (:commandId,:deviceId,:command,:argument,:operator,:requestTs)
            """, new MapSqlParameterSource("commandId", commandId).addValue("deviceId", deviceId)
            .addValue("command", command).addValue("argument", argument)
            .addValue("operator", operator).addValue("requestTs", requestTs));
    }

    public Optional<DiagnosticRequestBinding> diagnosticRequest(String commandId) {
        return jdbc.query("""
            SELECT device_id,command_name,argument,operator_name,request_ts
            FROM edge_diagnostic_request WHERE cmd_id=:commandId
            """, new MapSqlParameterSource("commandId", commandId),
            (rs, rowNum) -> new DiagnosticRequestBinding(rs.getLong("device_id"),
                rs.getString("command_name"), rs.getString("argument"),
                rs.getString("operator_name"), rs.getLong("request_ts"))).stream().findFirst();
    }

    public record DiagnosticRequestBinding(long deviceId, String command, String argument,
                                           String operator, long requestTs) { }

    public Optional<JsonNode> diagnosticResult(String machineCode, String commandId) {
        return jdbc.query("""
            SELECT r.payload_json FROM edge_diagnostic_reply r
            JOIN device_asset d ON d.id=r.device_id
            WHERE d.machine_code=:machineCode AND r.cmd_id=:commandId
            """, new MapSqlParameterSource("machineCode", machineCode)
            .addValue("commandId", commandId),
            (rs, rowNum) -> json(rs.getString("payload_json"))).stream().findFirst();
    }

    private JsonNode json(String value) {
        try {
            return objectMapper.readTree(value);
        } catch (JsonProcessingException exception) {
            return objectMapper.getNodeFactory().textNode(value);
        }
    }

    public void upsertPoints(List<PointValue> points, boolean fullSnapshot) {
        if (timescale == null || fullSnapshot) {
            upsertPoints(points);
            return;
        }
        if (points == null || points.isEmpty()) return;
        long deviceId = points.getFirst().deviceId();
        if (points.stream().anyMatch(point -> point.deviceId() != deviceId)) {
            throw new IllegalArgumentException("A telemetry snapshot must belong to one device");
        }
        // Interactive/legacy realtime may refresh latest, but cannot create periodic full history.
        timescale.upsert(machineCode(deviceId), points, false);
    }

    private String machineCode(long deviceId) {
        return jdbc.queryForObject("SELECT machine_code FROM device_asset WHERE id=:deviceId",
            new MapSqlParameterSource("deviceId", deviceId), String.class);
    }

    private void putText(com.fasterxml.jackson.databind.node.ObjectNode node, String field, String value) {
        if (value == null) node.putNull(field); else node.put(field, value);
    }

    public record PointValue(long deviceId, String meterCode, int pointIndex, String pointCode,
                             String pointName, String valueJson, Integer quality, long sourceTs,
                             Instant receivedAt, boolean stale, Boolean alarmActive, boolean noSample) {
        public PointValue(long deviceId, String meterCode, int pointIndex, String pointCode,
                          String pointName, String valueJson, Integer quality, long sourceTs,
                          Instant receivedAt, boolean stale, Boolean alarmActive) {
            this(deviceId, meterCode, pointIndex, pointCode, pointName, valueJson, quality,
                sourceTs, receivedAt, stale, alarmActive, false);
        }

        MapSqlParameterSource params() {
            return new MapSqlParameterSource()
                .addValue("deviceId", deviceId).addValue("meterCode", meterCode)
                .addValue("pointIndex", pointIndex).addValue("pointCode", pointCode)
                .addValue("pointName", pointName).addValue("valueJson", valueJson)
                .addValue("quality", quality).addValue("sourceTs", sourceTs)
                .addValue("receivedAt", Timestamp.from(receivedAt)).addValue("stale", stale)
                .addValue("alarmActive", alarmActive);
        }

        void addParams(MapSqlParameterSource params, String suffix) {
            params.addValue("deviceId" + suffix, deviceId)
                .addValue("meterCode" + suffix, meterCode)
                .addValue("pointIndex" + suffix, pointIndex)
                .addValue("pointCode" + suffix, pointCode)
                .addValue("pointName" + suffix, pointName)
                .addValue("valueJson" + suffix, valueJson)
                .addValue("quality" + suffix, quality)
                .addValue("sourceTs" + suffix, sourceTs)
                .addValue("receivedAt" + suffix, Timestamp.from(receivedAt))
                .addValue("stale" + suffix, stale)
                .addValue("alarmActive" + suffix, alarmActive);
        }
    }

    public record RealtimeLease(String sessionId, long deviceId, String machineCode,
                                String meterCode, Instant expiresAt, String state,
                                Instant updatedAt, String reasonCode) {
    }
}
