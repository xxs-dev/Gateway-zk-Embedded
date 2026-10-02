CREATE TABLE edge_diagnostic_request (
    cmd_id VARCHAR(128) PRIMARY KEY,
    device_id BIGINT NOT NULL REFERENCES device_asset(id),
    command_name VARCHAR(128) NOT NULL,
    argument VARCHAR(512) NOT NULL,
    operator_name VARCHAR(120) NOT NULL,
    request_ts BIGINT NOT NULL
);
