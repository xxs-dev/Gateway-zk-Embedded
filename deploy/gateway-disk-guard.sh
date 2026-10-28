#!/bin/sh
# Never delete business databases or queues. Keep diagnostics bounded.
set -eu
used=$(df -P /opt/modbus-gateway | awk 'NR==2 {gsub(/%/,"",$5); print $5}')
if [ "$used" -ge 75 ]; then
    journalctl --rotate --vacuum-size=256M >/dev/null 2>&1 || true
    used=$(df -P /opt/modbus-gateway | awk 'NR==2 {gsub(/%/,"",$5); print $5}')
    logger -p daemon.warning -t gateway-disk-guard "Disk usage ${used}%; threshold 75%, maximum target 80%; preserve business data"
fi
if [ "$used" -ge 80 ]; then
    logger -p daemon.crit -t gateway-disk-guard "Disk usage ${used}% exceeds target; archive history required; no business records deleted"
    exit 1
fi
