#!/bin/sh
set -eu

DEVICE_HOSTNAME="${DEVICE_HOSTNAME:-COMM202600105}"
VIRTUAL_IP="${VIRTUAL_IP:-}"
SOURCE_DIR="${SOURCE_DIR:-$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}"
INSTALL_BIN_DIR="${INSTALL_BIN_DIR:-/usr/local/bin}"
CONFIG_DIR="${CONFIG_DIR:-/etc/easytier}"
CONFIG_FILE="${CONFIG_FILE:-$CONFIG_DIR/et.conf}"
SERVICE_FILE="${SERVICE_FILE:-/etc/systemd/system/easytier.service}"

require_root() {
  if [ "$(id -u)" -ne 0 ]; then
    echo "This installer must run as root." >&2
    exit 1
  fi
}

validate_virtual_ip() {
  case "$VIRTUAL_IP" in
    *.*.*.*/*) ;;
    *.*.*.*) VIRTUAL_IP="$VIRTUAL_IP/24" ;;
    *)
      echo "VIRTUAL_IP is required, for example 10.126.126.12/24." >&2
      exit 2
      ;;
  esac
}

generate_uuid() {
  if command -v uuidgen >/dev/null 2>&1; then
    uuidgen
    return
  fi
  if [ -r /proc/sys/kernel/random/uuid ]; then
    cat /proc/sys/kernel/random/uuid
    return
  fi

  hex=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
  printf '%s-%s-%s-%s-%s\n' \
    "$(printf '%s' "$hex" | cut -c1-8)" \
    "$(printf '%s' "$hex" | cut -c9-12)" \
    "$(printf '%s' "$hex" | cut -c13-16)" \
    "$(printf '%s' "$hex" | cut -c17-20)" \
    "$(printf '%s' "$hex" | cut -c21-32)"
}

existing_uuid() {
  [ -f "$CONFIG_FILE" ] || return 1
  value=$(sed -n 's/^instance_id[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' "$CONFIG_FILE" | head -n 1)
  case "$value" in
    ????????-????-????-????-????????????)
      printf '%s\n' "$value"
      return 0
      ;;
  esac
  return 1
}

install_hostname() {
  if command -v hostnamectl >/dev/null 2>&1; then
    hostnamectl set-hostname "$DEVICE_HOSTNAME"
  else
    printf '%s\n' "$DEVICE_HOSTNAME" >/etc/hostname
    hostname "$DEVICE_HOSTNAME" 2>/dev/null || true
  fi
}

install_binaries() {
  for binary in easytier-core easytier-cli; do
    [ -f "$SOURCE_DIR/$binary" ] || {
      echo "Missing package file: $SOURCE_DIR/$binary" >&2
      exit 1
    }
  done

  machine=$(uname -m)
  case "$machine" in
    aarch64|arm64) ;;
    *)
      echo "Unsupported CPU architecture for this package: $machine" >&2
      exit 1
      ;;
  esac

  mkdir -p "$INSTALL_BIN_DIR" "$CONFIG_DIR"
  install -m 0755 "$SOURCE_DIR/easytier-core" "$INSTALL_BIN_DIR/easytier-core"
  install -m 0755 "$SOURCE_DIR/easytier-cli" "$INSTALL_BIN_DIR/easytier-cli"
}

write_config() {
  instance_id=$(existing_uuid || generate_uuid)
  umask 077
  cat >"$CONFIG_FILE" <<EOF
hostname = "$DEVICE_HOSTNAME"
instance_name = "kyxn"
instance_id = "$instance_id"
ipv4 = "$VIRTUAL_IP"
dhcp = false
listeners = [
    "tcp://0.0.0.0:11010",
    "udp://0.0.0.0:11010",
    "wg://0.0.0.0:11011",
]
rpc_portal = "0.0.0.0:0"

[network_identity]
network_name = "kyxn"
network_secret = "Kydl2025@"

[[peer]]
uri = "tcp://www.kyxn.net:11010"

[[peer]]
uri = "udp://www.kyxn.net:11010"

[flags]
private_mode = true
no_tun = true
EOF
  chmod 0600 "$CONFIG_FILE"
}

write_service() {
  cat >"$SERVICE_FILE" <<EOF
[Unit]
Description=EasyTier edge network (no TUN)
After=network-online.target gateway-cellular.service
Wants=network-online.target
StartLimitIntervalSec=0

[Service]
Type=simple
ExecStart=$INSTALL_BIN_DIR/easytier-core -c $CONFIG_FILE
Restart=always
RestartSec=5
User=root
LimitNOFILE=1048576

[Install]
WantedBy=multi-user.target
EOF
  chmod 0644 "$SERVICE_FILE"
}

start_service() {
  command -v systemctl >/dev/null 2>&1 || {
    echo "systemd is required by this installer." >&2
    exit 1
  }

  systemctl daemon-reload
  systemctl enable --now easytier.service
  sleep 2
  if ! systemctl is-active --quiet easytier.service; then
    systemctl --no-pager --full status easytier.service || true
    journalctl -u easytier.service -n 50 --no-pager || true
    exit 1
  fi
}

show_result() {
  instance_id=$(existing_uuid)
  echo "EasyTier installed successfully."
  echo "hostname=$DEVICE_HOSTNAME"
  echo "instance_id=$instance_id"
  echo "ipv4=$VIRTUAL_IP"
  echo "config=$CONFIG_FILE"
  echo "mode=no_tun"
  systemctl --no-pager --full status easytier.service | sed -n '1,12p'
}

require_root
validate_virtual_ip
install_hostname
install_binaries
write_config
write_service
start_service
show_result
