#!/usr/bin/env python3
"""Roll a legacy gateway to the current runtime while keeping both MQTT topics alive."""

import argparse
import hashlib
import json
import pathlib
import shlex
import threading
import time

import paramiko
import paho.mqtt.client as mqtt


class MqttHold:
    def __init__(self, host, port, username, password, machine_code):
        self.topics = {
            f"edge/telemetry/full/{machine_code}": 0.8,
            f"ky/peidian/{machine_code}": 8.0,
        }
        self.payloads = {}
        self.hashes = {}
        self.changed_at = {}
        self.received_at = {topic: [] for topic in self.topics}
        self.lock = threading.Lock()
        self.stop_event = threading.Event()
        self.client = mqtt.Client(
            mqtt.CallbackAPIVersion.VERSION2,
            client_id=f"runtime-upgrade-hold-{machine_code}-{int(time.time())}",
        )
        self.client.username_pw_set(username, password)
        self.client.on_connect = self._on_connect
        self.client.on_message = self._on_message
        self.client.connect(host, port, 30)
        self.client.loop_start()
        self.publisher = threading.Thread(target=self._publish_loop, daemon=True)

    def _on_connect(self, client, _userdata, _flags, _reason_code, _properties=None):
        for topic in self.topics:
            client.subscribe(topic, 1)

    def _on_message(self, _client, _userdata, message):
        now = int(time.time() * 1000)
        digest = hashlib.sha256(message.payload).hexdigest()
        with self.lock:
            self.received_at[message.topic].append(now)
            if self.hashes.get(message.topic) != digest:
                self.hashes[message.topic] = digest
                self.changed_at[message.topic] = now
            self.payloads[message.topic] = bytes(message.payload)

    def wait_baseline(self, timeout=35):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if all(topic in self.payloads for topic in self.topics):
                    self.publisher.start()
                    return
            time.sleep(0.1)
        raise RuntimeError("both MQTT baseline payloads were not received")

    def _publish_loop(self):
        next_publish = {topic: time.time() for topic in self.topics}
        while not self.stop_event.is_set():
            now = time.time()
            for topic, interval in self.topics.items():
                if now < next_publish[topic]:
                    continue
                with self.lock:
                    payload = self.payloads.get(topic)
                if payload:
                    self.client.publish(topic, payload, qos=1, retain=False)
                next_publish[topic] = now + interval
            self.stop_event.wait(0.05)

    def wait_fresh_after(self, mark_ms, timeout=90):
        deadline = time.time() + timeout
        while time.time() < deadline:
            with self.lock:
                if all(self.changed_at.get(topic, 0) > mark_ms for topic in self.topics):
                    return
            time.sleep(0.2)
        with self.lock:
            state = {topic: self.changed_at.get(topic, 0) for topic in self.topics}
        raise RuntimeError(f"new runtime did not produce fresh payloads on both MQTT topics: {state}")

    def stop_holding(self):
        self.stop_event.set()
        if self.publisher.is_alive():
            self.publisher.join(timeout=2)

    def verify_direct(self, timeout=22):
        with self.lock:
            starts = {topic: len(values) for topic, values in self.received_at.items()}
        time.sleep(timeout)
        result = {}
        with self.lock:
            for topic, values in self.received_at.items():
                direct = values[starts[topic]:]
                gaps = [right - left for left, right in zip(direct, direct[1:])]
                result[topic] = {
                    "messages": len(direct),
                    "maxGapMs": max(gaps) if gaps else None,
                }
        main_topic, legacy_topic = self.topics
        if result[main_topic]["messages"] < 10 or result[legacy_topic]["messages"] < 2:
            raise RuntimeError(f"direct MQTT verification failed: {result}")
        return result

    def close(self):
        self.stop_holding()
        self.client.loop_stop()
        self.client.disconnect()


def connect(host, password):
    client = paramiko.SSHClient()
    client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    client.connect(host, username="root", password=password, timeout=10)
    return client


def run(client, command, timeout=180):
    _stdin, stdout, stderr = client.exec_command(command, timeout=timeout)
    output = stdout.read().decode(errors="replace")
    error = stderr.read().decode(errors="replace")
    status = stdout.channel.recv_exit_status()
    if status != 0:
        raise RuntimeError(f"remote command failed ({status})\n{output}\n{error}")
    return output


def read_config(client):
    sftp = client.open_sftp()
    with sftp.open("/opt/modbus-gateway/config/runtime/apps/mqtt-service.json", "r") as source:
        config = json.loads(source.read().decode("utf-8"))
    with sftp.open("/etc/easytier/et.conf", "r") as source:
        easytier = source.read().decode("utf-8")
    sftp.close()
    return config, easytier


def rollback_runtime(client, backup):
    quoted = shlex.quote(backup)
    run(client, f"""set -eu
d={quoted}
units=$(tr '\n' ' ' < "$d/state/active-units.txt")
[ -z "$units" ] || systemctl stop $units
for source in "$d"/bin/*; do
    [ -f "$source" ] || continue
    name=$(basename "$source")
    install -m 0755 "$source" "/opt/modbus-gateway/bin/$name.restore"
    mv -f "/opt/modbus-gateway/bin/$name.restore" "/opt/modbus-gateway/bin/$name"
done
if [ -f "$d/state/realtime-ring.before" ]; then
    ring=$(python3 - <<'PY'
import json
config=json.load(open('/opt/modbus-gateway/config/runtime/apps/mqtt-service.json', encoding='utf-8'))
print(((config.get('mqtt') or {{}}).get('offlineBuffer') or {{}}).get('realtimeFile') or '')
PY
    )
    [ -z "$ring" ] || cp -p "$d/state/realtime-ring.before" "$ring"
fi
rm -f /dev/shm/gateway_point_store*
[ -z "$units" ] || systemctl start $units
""", timeout=90)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", required=True)
    parser.add_argument("--machine-code", required=True)
    parser.add_argument("--virtual-ip", required=True)
    parser.add_argument("--password", default="root")
    parser.add_argument("--package", required=True)
    parser.add_argument("--upgrade-script", required=True)
    parser.add_argument("--reset-realtime-ring", action="store_true")
    args = parser.parse_args()

    package = pathlib.Path(args.package)
    upgrade_script = pathlib.Path(args.upgrade_script)
    client = connect(args.host, args.password)
    config, easytier = read_config(client)
    identity = json.loads(run(client, "cat /opt/modbus-gateway/config/runtime/device_identity.json"))
    if identity.get("machineCode") != args.machine_code:
        raise RuntimeError(f"machineCode mismatch: {identity.get('machineCode')}")
    required_easytier = [
        f'hostname = "{args.machine_code}"',
        "dhcp = false",
        f'ipv4 = "{args.virtual_ip}/24"',
    ]
    if not all(value in easytier for value in required_easytier):
        raise RuntimeError("EasyTier fixed hostname/IP preflight failed")
    mqtt_config = config.get("mqtt") or {}
    forward_config = config.get("mqttForward") or {}
    if mqtt_config.get("broker") != forward_config.get("broker"):
        raise RuntimeError("this rollout only consolidates same-broker legacy outputs")

    broker = str(mqtt_config["broker"]).removeprefix("tcp://")
    broker_host, broker_port = broker.rsplit(":", 1)
    hold = MqttHold(
        broker_host,
        int(broker_port),
        str(mqtt_config.get("username") or ""),
        str(mqtt_config.get("password") or ""),
        args.machine_code,
    )
    backup = None
    try:
        hold.wait_baseline()
        sftp = client.open_sftp()
        sftp.put(str(package), "/tmp/gateway-runtime-v9.tar.gz")
        sftp.put(str(upgrade_script), "/tmp/upgrade-legacy-runtime-v9.sh")
        sftp.close()
        output = run(client, f"""set -eu
chmod 755 /tmp/upgrade-legacy-runtime-v9.sh
sh -n /tmp/upgrade-legacy-runtime-v9.sh
rm -rf /tmp/gateway-runtime-v9
tar -C /tmp -xzf /tmp/gateway-runtime-v9.tar.gz
(cd /tmp/gateway-runtime-v9 && sha256sum -c SHA256SUMS)
p=/opt/modbus-gateway/config/runtime/apps/mqtt-service.json
stamp=$(date +%Y%m%d-%H%M%S)
b=/opt/modbus-gateway/data/config-backups/{args.machine_code}-single-mqtt-$stamp
mkdir -p "$b"
cp -p "$p" "$b/mqtt-service.json.before"
python3 - "$p" <<'PY'
import json, os, sys
p=sys.argv[1]
d=json.load(open(p, encoding='utf-8'))
d['mqtt']['legacyTelemetryEnabled']=True
d['mqttForward']['enabled']=False
t=p+'.tmp'
with open(t, 'w', encoding='utf-8') as f:
    json.dump(d, f, ensure_ascii=False, indent=2)
    f.write('\\n')
os.replace(t, p)
PY
systemctl disable mqtt-forwarder@mqtt-service.service >/dev/null 2>&1 || true
systemctl stop mqtt-forwarder@mqtt-service.service
systemctl restart mqtt-driver@mqtt-service.service
EXPECTED_MACHINE_CODE={shlex.quote(args.machine_code)} STAGE_DIR=/tmp/gateway-runtime-v9 \
  HEALTH_TIMEOUT_SEC=120 MIN_RECOVERY_PERCENT=40 EXTERNAL_CONTINUITY=1 \
  RESET_REALTIME_RING={1 if args.reset_realtime_ring else 0} \
  /tmp/upgrade-legacy-runtime-v9.sh
""", timeout=220)
        for line in output.splitlines():
            if line.startswith("backup="):
                backup = line.split("=", 1)[1]
        mark = int(time.time() * 1000)
        hold.wait_fresh_after(mark)
        time.sleep(15)
        hold.stop_holding()
        direct = hold.verify_direct()
        final = run(client, """set -eu
for f in /dev/shm/gateway_point_store*; do
  [ "$(od -An -tx4 -N8 "$f" | awk '{printf "%s%s", $1, $2}')" = 4d50535400000009 ]
done
systemctl is-active mqtt-driver@mqtt-service.service >/dev/null
test "$(systemctl is-enabled mqtt-forwarder@mqtt-service.service 2>/dev/null || true)" = disabled
grep -q 'dhcp = false' /etc/easytier/et.conf
""")
        print(json.dumps({
            "machineCode": args.machine_code,
            "host": args.host,
            "backup": backup,
            "mqtt": direct,
            "status": "upgraded",
        }, ensure_ascii=False))
    except Exception:
        if backup:
            rollback_runtime(client, backup)
        raise
    finally:
        hold.close()
        client.close()


if __name__ == "__main__":
    main()
