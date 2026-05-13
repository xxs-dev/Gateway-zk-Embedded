#!/usr/bin/env python3
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time
import urllib.request

os.environ.setdefault("WEBKIT_DISABLE_COMPOSITING_MODE", "1")
os.environ.setdefault("WEBKIT_DISABLE_DMABUF_RENDERER", "1")
os.environ.setdefault("GDK_GL", "disable")
os.environ.setdefault("LIBGL_ALWAYS_SOFTWARE", "1")

import gi

gi.require_version("Gtk", "3.0")
gi.require_version("Gdk", "3.0")
from gi.repository import Gdk, GLib, Gtk


DEFAULT_KIOSK = {
    "width": 1280,
    "height": 720,
    "fullscreen": True,
    "setDisplayMode": True,
    "enabled": True,
    "requireDisplayConnected": True,
    "requireEdid": True,
    "displayOutput": "",
    "zoom": 1.0,
}


def log(message):
    print("[local-kiosk] {}".format(message), flush=True)


def bool_value(value, default=False):
    if isinstance(value, bool):
        return value
    if value is None:
        return default
    return str(value).strip().lower() in ("1", "true", "yes", "on")


def int_value(value, default):
    try:
        parsed = int(value)
        return parsed if parsed > 0 else default
    except Exception:
        return default


def float_value(value, default):
    try:
        parsed = float(value)
        return parsed if parsed > 0 else default
    except Exception:
        return default


def normalize_kiosk_config(raw):
    kiosk = dict(DEFAULT_KIOSK)
    if isinstance(raw, dict):
        kiosk["width"] = int_value(raw.get("width"), kiosk["width"])
        kiosk["height"] = int_value(raw.get("height"), kiosk["height"])
        kiosk["fullscreen"] = bool_value(raw.get("fullscreen"), kiosk["fullscreen"])
        kiosk["setDisplayMode"] = bool_value(raw.get("setDisplayMode"), kiosk["setDisplayMode"])
        kiosk["enabled"] = bool_value(raw.get("enabled"), kiosk["enabled"])
        kiosk["requireDisplayConnected"] = bool_value(raw.get("requireDisplayConnected"), kiosk["requireDisplayConnected"])
        kiosk["requireEdid"] = bool_value(raw.get("requireEdid"), kiosk["requireEdid"])
        kiosk["displayOutput"] = str(raw.get("displayOutput") or "").strip()
        kiosk["zoom"] = float_value(raw.get("zoom"), kiosk["zoom"])
    return kiosk


def load_local_display_settings(app_config_path):
    host = "127.0.0.1"
    port = 18080
    page_code = "overview"
    kiosk = dict(DEFAULT_KIOSK)
    try:
        with open(app_config_path, "r", encoding="utf-8") as fh:
            root = json.load(fh)
        local_display = root.get("localDisplay") or {}
        # The kiosk always talks to the local loopback endpoint. bindHost may be
        # 0.0.0.0 for LAN access, but the local process should still use 127.0.0.1.
        port = int_value(local_display.get("port"), port)
        kiosk = normalize_kiosk_config(local_display.get("kiosk") or {})
        pages = local_display.get("pages") or []
        if pages:
            page_code = pages[0].get("pageCode") or page_code
    except Exception as exc:
        log("failed to load app config {}, using defaults: {}".format(app_config_path, exc))
    page_url = "http://{}:{}/?pageCode={}".format(host, port, page_code)
    frame_url = "http://{}:{}/api/frame?pageCode={}".format(host, port, page_code)
    return page_url, frame_url, kiosk


def find_xrandr_output(xrandr_path):
    try:
        result = subprocess.run(
            [xrandr_path, "--query"],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            timeout=2,
        )
    except Exception as exc:
        log("xrandr query failed: {}".format(exc))
        return ""
    if result.returncode != 0:
        log("xrandr query returned {}: {}".format(result.returncode, result.stderr.strip()))
        return ""
    connected = []
    for line in result.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 2 and parts[1] == "connected":
            name = parts[0]
            if "primary" in parts:
                return name
            connected.append(name)
    return connected[0] if connected else ""


def connector_has_edid(status_path):
    edid = os.path.join(os.path.dirname(status_path), "edid")
    try:
        return os.path.isfile(edid) and os.path.getsize(edid) > 0
    except Exception:
        return False


def display_connected_by_sysfs(expected_output="", require_edid=True):
    patterns = [
        "/sys/class/drm/card*-HDMI*/status",
        "/sys/class/drm/card*-DP*/status",
        "/sys/class/drm/card*-DPI*/status",
        "/sys/class/drm/card*-DSI*/status",
        "/sys/class/drm/card*-LVDS*/status",
        "/sys/class/drm/card*-VGA*/status",
    ]
    for pattern in patterns:
        for path in glob.glob(pattern):
            name = os.path.basename(os.path.dirname(path))
            if expected_output and not name.endswith("-" + expected_output):
                continue
            try:
                with open(path, "r", encoding="utf-8") as fh:
                    if fh.read().strip().lower() == "connected" and (not require_edid or connector_has_edid(path)):
                        return True
            except Exception:
                pass
    return False


def display_connected(kiosk):
    if not bool_value(kiosk.get("requireDisplayConnected"), True):
        return True
    xrandr_path = shutil.which("xrandr")
    expected = str(kiosk.get("displayOutput") or "").strip()
    require_edid = bool_value(kiosk.get("requireEdid"), False if expected else True)
    if display_connected_by_sysfs(expected, require_edid):
        return True
    if require_edid:
        return False
    if xrandr_path:
        if expected:
            code, out, _err = run_xrandr([xrandr_path, "--query"], timeout=2)
            if code == 0:
                for line in out.splitlines():
                    parts = line.split()
                    if len(parts) >= 2 and parts[0] == expected and parts[1] == "connected":
                        return True
            return False
        elif find_xrandr_output(xrandr_path):
            return True
    return False


def run_xrandr(cmd, timeout=3):
    try:
        result = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            universal_newlines=True,
            timeout=timeout,
        )
    except Exception as exc:
        return 1, "", str(exc)
    return result.returncode, result.stdout.strip(), result.stderr.strip()


def ensure_720p_mode(xrandr_path, output):
    mode_name = "1280x720_60.00"
    newmode = [
        xrandr_path,
        "--newmode",
        mode_name,
        "74.50",
        "1280",
        "1344",
        "1472",
        "1664",
        "720",
        "723",
        "728",
        "748",
        "-hsync",
        "+vsync",
    ]
    code, out, err = run_xrandr(newmode)
    if code != 0 and "already exists" not in err:
        log("failed to create 720p xrandr mode: {}".format(err or out))
        return ""
    addmode = [xrandr_path, "--addmode", output, mode_name]
    code, out, err = run_xrandr(addmode)
    if code != 0 and "already exists" not in err:
        log("failed to add 720p xrandr mode to {}: {}".format(output, err or out))
        return ""
    return mode_name


def set_display_mode(kiosk):
    if not bool_value(kiosk.get("setDisplayMode"), True):
        return
    xrandr_path = shutil.which("xrandr")
    if not xrandr_path:
        log("xrandr not found, skip display mode change")
        return
    output = str(kiosk.get("displayOutput") or "").strip() or find_xrandr_output(xrandr_path)
    if not output:
        log("no connected xrandr output found, skip display mode change")
        return
    width = int_value(kiosk.get("width"), 1280)
    height = int_value(kiosk.get("height"), 720)
    mode = "{}x{}".format(width, height)
    cmd = [xrandr_path, "--output", output, "--mode", mode]
    code, out, err = run_xrandr(cmd)
    if code == 0:
        log("display mode set: output={} mode={}".format(output, mode))
        return
    if width == 1280 and height == 720:
        fallback_mode = ensure_720p_mode(xrandr_path, output)
        if fallback_mode:
            code, out, err = run_xrandr([xrandr_path, "--output", output, "--mode", fallback_mode])
            if code == 0:
                log("display mode set: output={} mode={}".format(output, fallback_mode))
                return
    log("display mode change failed: output={} mode={} error={}".format(output, mode, err or out))


def find_browser():
    candidates = [
        ("chromium", ["--kiosk", "--no-sandbox", "--disable-infobars"]),
        ("chromium-browser", ["--kiosk", "--no-sandbox", "--disable-infobars"]),
        ("google-chrome", ["--kiosk", "--no-sandbox", "--disable-infobars"]),
        ("firefox", ["--kiosk"]),
        ("midori", ["-e", "Fullscreen"]),
        ("epiphany-browser", ["--fullscreen"]),
        ("cog", []),
    ]
    for name, args in candidates:
        path = shutil.which(name)
        if path:
            return [path] + args
    return None


def try_run_browser(url):
    cmd = find_browser()
    if not cmd:
        return False
    log("starting browser renderer: {}".format(cmd[0]))
    subprocess.call(cmd + [url])
    return True


class WebKiosk(Gtk.Window):
    def __init__(self, url, webkit, kiosk):
        Gtk.Window.__init__(self, title="Gateway Local Display")
        width = int_value(kiosk.get("width"), 1280)
        height = int_value(kiosk.get("height"), 720)
        zoom = float_value(kiosk.get("zoom"), 1.0)
        fullscreen = bool_value(kiosk.get("fullscreen"), True)

        self.set_decorated(False)
        self.set_default_size(width, height)
        self.set_size_request(width, height)
        if fullscreen:
            self.fullscreen()
        self.connect("destroy", Gtk.main_quit)
        self.connect("key-press-event", self.on_key)
        self.kiosk = kiosk

        webview = webkit.WebView()
        if hasattr(webview, "set_zoom_level"):
            webview.set_zoom_level(zoom)
        webview.load_uri(url)
        self.add(webview)
        GLib.timeout_add(5000, self.check_display)

    def on_key(self, _widget, event):
        key = Gdk.keyval_name(event.keyval)
        if key in ("Escape", "q", "Q"):
            Gtk.main_quit()
        elif key == "F11":
            self.fullscreen()

    def check_display(self):
        if not display_connected(self.kiosk):
            log("display disconnected, stopping WebKit renderer")
            Gtk.main_quit()
            return False
        return True


def try_run_webkit(url, kiosk):
    try:
        gi.require_version("WebKit2", "4.0")
        from gi.repository import WebKit2
    except Exception as exc:
        log("WebKit2 unavailable, fallback to browser/GTK: {}".format(exc))
        return False
    log("starting WebKit renderer")
    window = WebKiosk(url, WebKit2, kiosk)
    window.show_all()
    Gtk.main()
    return True


class Kiosk(Gtk.Window):
    def __init__(self, url, kiosk):
        Gtk.Window.__init__(self, title="Gateway Local Display")
        width = int_value(kiosk.get("width"), 1280)
        height = int_value(kiosk.get("height"), 720)
        fullscreen = bool_value(kiosk.get("fullscreen"), True)
        self.url = url
        self.kiosk = kiosk
        self.set_decorated(False)
        self.set_default_size(width, height)
        self.set_size_request(width, height)
        if fullscreen:
            self.fullscreen()
        self.connect("destroy", Gtk.main_quit)
        self.connect("key-press-event", self.on_key)

        css = b"""
        window { background: #101820; color: #e8f0f2; }
        label.title { font-size: 34px; font-weight: bold; color: #e8f0f2; }
        label.sub { font-size: 18px; color: #9fb2bc; }
        label.card-title { font-size: 16px; color: #9fb2bc; }
        label.card-num { font-size: 34px; font-weight: bold; color: #e8f0f2; }
        treeview { background: #17222c; color: #e8f0f2; font-size: 18px; }
        treeview:selected { background: #24516a; }
        """
        provider = Gtk.CssProvider()
        provider.load_from_data(css)
        Gtk.StyleContext.add_provider_for_screen(
            Gdk.Screen.get_default(),
            provider,
            Gtk.STYLE_PROVIDER_PRIORITY_APPLICATION
        )

        root = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
        root.set_margin_top(18)
        root.set_margin_bottom(18)
        root.set_margin_start(24)
        root.set_margin_end(24)
        self.add(root)

        top = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=12)
        root.pack_start(top, False, False, 0)
        self.title_label = Gtk.Label(label="Gateway Local Display")
        self.title_label.get_style_context().add_class("title")
        self.title_label.set_xalign(0)
        top.pack_start(self.title_label, True, True, 0)
        self.time_label = Gtk.Label(label="-")
        self.time_label.get_style_context().add_class("sub")
        self.time_label.set_xalign(1)
        top.pack_start(self.time_label, False, False, 0)

        cards = Gtk.Box(orientation=Gtk.Orientation.HORIZONTAL, spacing=14)
        root.pack_start(cards, False, False, 0)
        self.card_points = self.card(cards, "点位数量")
        self.card_online = self.card(cards, "正常点位")
        self.card_bad = self.card(cards, "异常/过期")
        self.card_seq = self.card(cards, "刷新序号")

        self.store = Gtk.ListStore(str, str, str, str, str, str, str, str)
        self.last_points_signature = None
        tree = Gtk.TreeView(model=self.store)
        headers = ["Index", "Meter", "Point", "Name", "Value", "Unit", "Quality", "Time"]
        for i, header in enumerate(headers):
            renderer = Gtk.CellRendererText()
            column = Gtk.TreeViewColumn(header, renderer, text=i)
            column.set_resizable(True)
            column.set_min_width(110 if i != 3 else 220)
            tree.append_column(column)
        scroll = Gtk.ScrolledWindow()
        scroll.set_policy(Gtk.PolicyType.AUTOMATIC, Gtk.PolicyType.AUTOMATIC)
        scroll.add(tree)
        root.pack_start(scroll, True, True, 0)

        GLib.timeout_add(1000, self.refresh)
        GLib.timeout_add(5000, self.check_display)
        self.refresh()

    def card(self, parent, title):
        box = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=4)
        box.set_size_request(220, 90)
        title_label = Gtk.Label(label=title)
        title_label.get_style_context().add_class("card-title")
        title_label.set_xalign(0)
        num = Gtk.Label(label="0")
        num.get_style_context().add_class("card-num")
        num.set_xalign(0)
        box.pack_start(title_label, False, False, 0)
        box.pack_start(num, False, False, 0)
        parent.pack_start(box, False, False, 0)
        return num

    def on_key(self, _widget, event):
        key = Gdk.keyval_name(event.keyval)
        if key in ("Escape", "q", "Q"):
            Gtk.main_quit()
        elif key == "F11":
            self.fullscreen()

    def check_display(self):
        if not display_connected(self.kiosk):
            log("display disconnected, stopping GTK renderer")
            Gtk.main_quit()
            return False
        return True

    def refresh(self):
        try:
            with urllib.request.urlopen(self.url, timeout=1.5) as resp:
                data = json.loads(resp.read().decode("utf-8"))
            points = data.get("points") or []
            ok = [p for p in points if p.get("hasValue") and p.get("quality") == 1 and not p.get("stale")]
            bad = [p for p in points if (not p.get("hasValue")) or p.get("quality") != 1 or p.get("stale")]
            self.title_label.set_text(
                "Gateway Local Display   machine={}".format(data.get("machineCode") or "-")
            )
            self.time_label.set_text(time.strftime("%Y-%m-%d %H:%M:%S"))
            self.card_points.set_text(str(len(points)))
            self.card_online.set_text(str(len(ok)))
            self.card_bad.set_text(str(len(bad)))
            self.card_seq.set_text(str(data.get("seq") or "-"))
            signature = tuple(
                (
                    point.get("index"),
                    point.get("value"),
                    point.get("quality"),
                    point.get("valueTs"),
                    point.get("stale"),
                )
                for point in points[:200]
            )
            if signature == self.last_points_signature:
                return True
            self.last_points_signature = signature
            self.store.clear()
            for point in points[:200]:
                value = point.get("value")
                if value is None:
                    value_text = "-"
                else:
                    value_text = ("%.3f" % float(value)).rstrip("0").rstrip(".")
                ts = point.get("valueTs") or 0
                time_text = "-" if not ts else time.strftime("%H:%M:%S", time.localtime(ts / 1000.0))
                self.store.append([
                    str(point.get("index") or ""),
                    str(point.get("meterCode") or ""),
                    str(point.get("pointCode") or ""),
                    str(point.get("name") or ""),
                    value_text,
                    str(point.get("unit") or ""),
                    str(point.get("quality") or ""),
                    time_text,
                ])
        except Exception as exc:
            self.title_label.set_text("Gateway Local Display   error={}".format(exc))
        return True


def apply_cli_overrides(kiosk, args):
    if args.width:
        kiosk["width"] = int_value(args.width, kiosk["width"])
    if args.height:
        kiosk["height"] = int_value(args.height, kiosk["height"])
    if args.display_output:
        kiosk["displayOutput"] = args.display_output.strip()
    if args.zoom:
        kiosk["zoom"] = float_value(args.zoom, kiosk["zoom"])
    if args.windowed:
        kiosk["fullscreen"] = False
    if args.no_set_display_mode:
        kiosk["setDisplayMode"] = False
    return kiosk


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--app-config", default="/opt/modbus-gateway/config/runtime/apps/monitor-service.json")
    parser.add_argument("--width", type=int)
    parser.add_argument("--height", type=int)
    parser.add_argument("--windowed", action="store_true")
    parser.add_argument("--no-set-display-mode", action="store_true")
    parser.add_argument("--display-output", default="")
    parser.add_argument("--zoom", type=float)
    args = parser.parse_args()

    page_url, frame_url, kiosk = load_local_display_settings(args.app_config)
    kiosk = apply_cli_overrides(kiosk, args)
    log("page url={}".format(page_url))
    log("kiosk width={} height={} fullscreen={} setDisplayMode={} output={} zoom={}".format(
        kiosk.get("width"),
        kiosk.get("height"),
        kiosk.get("fullscreen"),
        kiosk.get("setDisplayMode"),
        kiosk.get("displayOutput") or "auto",
        kiosk.get("zoom"),
    ))
    if not bool_value(kiosk.get("enabled"), True):
        log("kiosk disabled by localDisplay.kiosk.enabled=false")
        return
    if not display_connected(kiosk):
        log("no connected display output, skip local kiosk")
        return
    set_display_mode(kiosk)
    if try_run_webkit(page_url, kiosk):
        return
    if try_run_browser(page_url):
        return
    log("no HTML renderer found, fallback to GTK table")
    window = Kiosk(frame_url, kiosk)
    window.show_all()
    Gtk.main()


if __name__ == "__main__":
    main()
