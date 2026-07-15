# ZLMediaKit 平台媒体服务接入教程

## 目标

本方案使用 ZLMediaKit 作为平台公网媒体入口，解决“摄像头和边端在现场局域网、平台在公网”的视频访问问题。

生产链路：

```text
现场摄像头/USB 摄像头
        |
        | 现场局域网拉流
        v
边端 CameraService
        |
        | 主动出站推 RTSP
        v
公网服务器 ZLMediaKit
        |
        | HLS / HTTP-FLV / WebRTC / RTSP
        v
Java 平台页面 / VLC / 浏览器播放器
```

关键边界：

- 视频流不走 MQTT。
- 视频流不写共享内存。
- 边端只主动向公网推流，不要求边端有公网 IP。
- Java 平台不直接承载视频流，只负责配置、鉴权、状态、播放地址和 OTA。
- ZLMediaKit 负责接收推流、协议转换、录像、截图和播放出口。

## 选型理由

ZLMediaKit 适合作为当前项目的平台媒体服务：

- 支持 RTSP 推流，边端 `CameraService` 可以用 `ffmpeg` 直接推到平台。
- 支持 RTSP、RTMP、HLS、HTTP-FLV、WebSocket-FLV、HTTP-TS、HTTP-fMP4、WebRTC 等协议转换。
- 提供 RESTful API，可查询流列表、启动录像、停止录像、截图。
- 提供 WebHook，可做推流鉴权、播放鉴权、流上线/下线通知。
- Docker 镜像可快速部署，适合先联调再生产固化。

## 推荐部署拓扑

公网服务器建议部署：

```text
Nginx :443
  |
  +-- /              -> Java 平台
  +-- /video/        -> ZLMediaKit HTTP/HLS/HTTP-FLV
  +-- /zlm-api/      -> ZLMediaKit API，必须内网或平台后端访问，不建议公网裸露

ZLMediaKit
  |
  +-- TCP 8554       RTSP 推流入口，边端访问
  +-- TCP 8088       HTTP/HLS/HTTP-FLV，建议只给 Nginx 反代
  +-- TCP/UDP 10000  WebRTC，低延迟播放时使用
```

端口规划：

| 端口 | 方向 | 用途 |
| --- | --- | --- |
| `8554/tcp` | 边端 -> 平台 | RTSP 推流入口 |
| `8088/tcp` | Nginx -> ZLMediaKit | HLS、HTTP-FLV、HTTP-fMP4、API |
| `10000/tcp/udp` | 浏览器 -> 平台 | WebRTC，低延迟播放需要 |
| `443/tcp` | 浏览器 -> 平台 | HTTPS 访问 Java 平台和播放地址 |

生产建议：

- `8554` 只允许边端出口 IP 或 VPN 网段访问。
- `8088` 不直接暴露公网，只由 Nginx 反代。
- `/index/api/*` 必须带 `secret`，并且最好只允许 Java 后端访问。
- 播放鉴权和推流鉴权最终通过 WebHook 接入 Java 平台。

## 快速部署

以下命令在平台公网服务器执行。

创建目录：

```bash
mkdir -p /opt/zlmediakit/{conf,www,logs,record}
cd /opt/zlmediakit
```

启动临时容器复制默认配置：

```bash
docker run --rm \
  -v /opt/zlmediakit/conf:/out \
  zlmediakit/zlmediakit:master \
  sh -c 'cp -f /opt/media/conf/config.ini /out/config.ini || cp -f /opt/media/bin/config.ini /out/config.ini'
```

如果镜像内路径因版本变化导致复制失败，可以先交互查看：

```bash
docker run --rm -it zlmediakit/zlmediakit:master sh
find / -name config.ini 2>/dev/null
```

创建 `docker-compose.yml`：

```yaml
services:
  zlmediakit:
    image: zlmediakit/zlmediakit:master
    container_name: zlmediakit
    restart: unless-stopped
    network_mode: host
    volumes:
      - /opt/zlmediakit/conf/config.ini:/opt/media/conf/config.ini:ro
      - /opt/zlmediakit/www:/opt/media/bin/www
      - /opt/zlmediakit/logs:/opt/media/bin/log
      - /opt/zlmediakit/record:/opt/media/bin/record
```

启动：

```bash
docker compose up -d
docker logs -f zlmediakit
```

验证 API：

```bash
curl 'http://127.0.0.1:8088/index/api/getApiList?secret=change_this_secret'
```

如果当前配置的 HTTP 端口不是 `8088`，按 `config.ini` 中 `[http].port` 实际值调整。

## config.ini 关键配置

以下只列当前项目相关的关键项，实际文件保留其他默认配置。

```ini
[api]
apiDebug=0
secret=change_this_secret

[general]
enableVhost=0
flowThreshold=1024
streamNoneReaderDelayMS=30000

[http]
port=8088
sslport=8443
rootPath=./www

[rtsp]
port=8554
sslport=8322
authBasic=0

[rtmp]
port=1935

[hls]
segDur=2
segNum=3
filePath=./www

[protocol]
enable_hls=1
enable_hls_fmp4=1
enable_rtsp=1
enable_rtmp=1
enable_ts=1
enable_fmp4=1
hls_demand=0
rtsp_demand=0
rtmp_demand=0
ts_demand=0
fmp4_demand=0

[record]
filePath=./record
fileSecond=3600

[hook]
enable=1
timeoutSec=10
admin_params=secret=change_this_secret
on_publish=http://127.0.0.1:8090/api/zlm/hook/on_publish
on_play=http://127.0.0.1:8090/api/zlm/hook/on_play
on_stream_changed=http://127.0.0.1:8090/api/zlm/hook/on_stream_changed
on_flow_report=http://127.0.0.1:8090/api/zlm/hook/on_flow_report
on_server_keepalive=http://127.0.0.1:8090/api/zlm/hook/on_server_keepalive
```

说明：

- `secret` 是 ZLMediaKit REST API 管理密钥，不能使用默认值。
- `on_publish` 用于边端推流鉴权。
- `on_play` 用于浏览器播放鉴权。
- `on_stream_changed` 用于流上线/下线同步到 Java 平台。
- 第一阶段 Java hook 可以先返回允许，联调稳定后再加账号、token、路径 ACL。

## 边端配置

边端 `runtime/apps/camera-service.json` 推荐结构：

```json
{
  "cameraService": {
    "enabled": true,
    "statusIntervalMs": 5000,
    "sharedMemoryName": "gateway_point_store",
    "statusTopic": "edge/camera/status",
    "eventTopic": "edge/camera/event",
    "media": {
      "type": "rtsp_push",
      "serverUrl": "rtsp://203.0.113.10:8554",
      "transport": "tcp",
      "reconnectIntervalMs": 5000,
      "auth": {
        "enabled": true,
        "mode": "token_query",
        "token": "edge_push_token_for_GW0001",
        "tokenParam": "token",
        "hideCredentialsInStatus": true
      }
    },
    "cameras": [
      {
        "cameraCode": "CAM001",
        "name": "入口摄像头",
        "enabled": true,
        "sourceType": "rtsp",
        "source": "rtsp://192.168.1.50:554/stream1",
        "sourceAuth": {
          "enabled": true,
          "mode": "basic",
          "username": "camera_user",
          "password": "camera_password",
          "hideCredentialsInStatus": true
        },
        "video": {
          "codec": "copy",
          "width": 1280,
          "height": 720,
          "fps": 15,
          "bitrateKbps": 1500
        },
        "stream": {
          "path": "GW0001/CAM001",
          "publishUrl": ""
        },
        "statusPointIndexes": {
          "online": 51000,
          "fps": 51001,
          "bitrateKbps": 51002,
          "errorCode": 51003
        }
      }
    ]
  }
}
```

总开关规则：

- `cameraService.enabled=false` 是出厂默认值，表示边端不启动摄像头服务。
- 只有 `cameraService.enabled=true` 且至少一个 `cameras[].enabled=true` 时，`gateway-services.sh` 才会启动 `camera-service@camera-service.service`。
- 服务关闭时 Java 平台“摄像头监控”只显示关闭状态，不查询 ZLMediaKit，不生成播放地址。
- 单个摄像头 `enabled=false` 时，该路不会推流，不参与 ZLMediaKit 状态查询。

生成的推流地址：

```text
rtsp://203.0.113.10:8554/GW0001/CAM001?token=edge_push_token_for_GW0001
```

ZLMediaKit 中这个流的四元组为：

| 字段 | 值 |
| --- | --- |
| schema | `rtsp` |
| vhost | `__defaultVhost__` |
| app | `GW0001` |
| stream | `CAM001` |

当前 Java 平台 `/api/camera/streams` 会按同样规则展示播放地址。

## 播放地址规则

以 `machineCode=GW0001`、`cameraCode=CAM001` 为例：

```text
RTSP:      rtsp://203.0.113.10:8554/GW0001/CAM001
HLS:       https://platform.example.com/video/GW0001/CAM001/hls.m3u8
HTTP-FLV:  https://platform.example.com/video/GW0001/CAM001.live.flv
HTTP-fMP4: https://platform.example.com/video/GW0001/CAM001.live.mp4
WebRTC:    https://platform.example.com/webrtc/
```

浏览器建议优先顺序：

1. 低延迟要求高：WebRTC。
2. 兼容性和实现简单：HLS。
3. 延迟比 HLS 低且播放器可控：HTTP-FLV 或 HTTP-fMP4。

第一阶段建议平台页面先展示 RTSP/HLS/HTTP-FLV 地址；浏览器内嵌播放可后续接入 `hls.js`、`mpegts.js` 或 ZLMediaKit WebRTC 测试页。

## Nginx HTTPS 反向代理

示例：

```nginx
server {
    listen 443 ssl http2;
    server_name platform.example.com;

    ssl_certificate     /etc/nginx/cert/platform.pem;
    ssl_certificate_key /etc/nginx/cert/platform.key;

    client_max_body_size 200m;

    location / {
        proxy_pass http://127.0.0.1:8090;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_set_header X-Forwarded-Proto https;
    }

    location /video/ {
        rewrite ^/video/(.*)$ /$1 break;
        proxy_pass http://127.0.0.1:8088;
        proxy_set_header Host $host;
        proxy_set_header X-Real-IP $remote_addr;
        proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
        proxy_buffering off;
        add_header Access-Control-Allow-Origin * always;
    }

    location /zlm-api/ {
        allow 127.0.0.1;
        allow 10.0.0.0/8;
        deny all;
        rewrite ^/zlm-api/(.*)$ /index/api/$1 break;
        proxy_pass http://127.0.0.1:8088;
        proxy_set_header Host $host;
    }
}
```

注意：

- `/video/` 用于播放，不直接暴露 `/index/api/`。
- `/zlm-api/` 只允许 Java 后端或内网访问。
- 如果要 WebRTC，还需要按实际部署开放 `10000/tcp` 和 `10000/udp`，并处理公网 NAT 映射。

## 推流测试

在平台服务器先用测试视频推流：

```bash
ffmpeg -re -i test.mp4 \
  -vcodec h264 -acodec aac \
  -f rtsp -rtsp_transport tcp \
  'rtsp://127.0.0.1:8554/GW0001/CAM001?token=edge_push_token_for_GW0001'
```

查询流是否在线：

```bash
curl 'http://127.0.0.1:8088/index/api/getMediaList?secret=change_this_secret&schema=rtsp&app=GW0001&stream=CAM001'
```

播放测试：

```bash
ffplay 'rtsp://127.0.0.1:8554/GW0001/CAM001'
```

浏览器访问：

```text
https://platform.example.com/video/GW0001/CAM001/hls.m3u8
https://platform.example.com/video/GW0001/CAM001.live.flv
```

## 边端联调流程

1. 在 Java 平台 `MQTT / OTA / App -> 摄像头` 编辑 `runtime/apps/camera-service.json`。
2. 设置 `cameraService.enabled=true`。
3. 至少启用一个 `cameras[].enabled=true` 的摄像头。
4. 设置 `media.serverUrl=rtsp://<平台公网IP>:8554`。
5. 每个摄像头设置 `stream.path=<machineCode>/<cameraCode>`。
6. 生成并发布配置 OTA。
7. 边端确认服务启动：

```bash
systemctl status camera-service@camera-service.service
journalctl -u camera-service@camera-service.service -f
```

8. 平台查询 ZLMediaKit：

```bash
curl 'http://127.0.0.1:8088/index/api/getMediaList?secret=change_this_secret&schema=rtsp&app=GW0001'
```

9. Java 平台打开“摄像头监控”，选择 `machineCode`，确认播放地址和状态点。

## Java 平台对接

当前 Java 平台已经落地：

- `runtime/apps/camera-service.json` 配置编辑。
- `/api/camera/streams` 摄像头清单、播放地址、ZLMediaKit 在线状态和共享内存状态点。
- `/api/camera/streams` 会尊重 `cameraService.enabled`，关闭时返回空 `streams` 且不访问 ZLMediaKit。
- 配置 OTA 默认包含 `camera-service.json`。
- `edge.gateway.media.zlm.*` 配置项。
- ZLMediaKit REST API 调用 `getMediaList`，用于判断流是否已推到平台。
- ZLMediaKit Hook 接口 `on_publish/on_play/on_stream_changed/on_flow_report/on_server_keepalive`。

### ZLMediaKit 配置项

```yaml
edge:
  gateway:
    media:
      zlm:
        enabled: true
        base-url: http://127.0.0.1:8088
        public-base-url: https://platform.example.com/video
        secret: change_this_secret
        default-vhost: __defaultVhost__
        push-token: edge_push_token_for_GW0001
        play-token: viewer_token
        connect-timeout-ms: 2000
        read-timeout-ms: 3000
```

字段说明：

| 字段 | 说明 |
| --- | --- |
| `enabled` | 是否启用 ZLMediaKit 联动；关闭时摄像头页只显示配置推导地址 |
| `base-url` | Java 后端访问 ZLMediaKit 的内网地址 |
| `public-base-url` | 浏览器播放使用的公网地址，通常是 Nginx `/video` 反代地址 |
| `secret` | ZLMediaKit `[api].secret` |
| `default-vhost` | 默认 `__defaultVhost__` |
| `push-token` | 边端推流 token，`on_publish` 校验 |
| `play-token` | 播放 token，`on_play` 校验 |

### 已实现平台 API

```http
GET /api/camera/streams?machineCode=GW0001&appName=runtime/apps/camera-service.json
```

该接口内部调用 ZLMediaKit：

```text
/index/api/getMediaList
```

返回内容包含：

- `zlmEnabled`: Java 平台是否启用 ZLMediaKit 联动。
- `zlmOnline`: 该摄像头对应的 ZLMediaKit 流是否在线。
- `zlmReaderCount`: 当前读者数。
- `zlmBytesSpeed`: ZLMediaKit 报告的流速率。
- `hlsUrl`: HLS 播放地址。
- `flvUrl`: HTTP-FLV 播放地址。
- `rtspUrl`: RTSP 播放地址。

录像、截图接口还未实现，后续可继续接入：

```http
POST /api/media/zlm/record/start
POST /api/media/zlm/record/stop
GET  /api/media/zlm/snap?machineCode=GW0001&cameraCode=CAM001
```

### 已实现 Hook 接口

```http
POST /api/zlm/hook/on_publish
POST /api/zlm/hook/on_play
POST /api/zlm/hook/on_stream_changed
POST /api/zlm/hook/on_flow_report
POST /api/zlm/hook/on_server_keepalive
```

当前行为：

- `on_publish`: `edge.gateway.media.zlm.push-token` 非空时校验 URL 参数 `token`，并要求 `app/stream` 非空。
- `on_play`: `edge.gateway.media.zlm.play-token` 非空时校验 URL 参数 `token`。
- `on_stream_changed`: 当前返回成功，后续可落库同步在线状态。
- `on_flow_report`: 当前返回成功，后续可落库流量。
- `on_server_keepalive`: 当前返回成功，后续可落库服务状态。

注意：当前 `push-token` 是平台全局配置。生产上建议升级为按 `machineCode` 或 `machineCode/cameraCode` 独立 token，并在 `on_publish` 中校验 `app == machineCode`、`stream == cameraCode`。

## 鉴权策略

推荐使用双 token：

| 场景 | token 来源 | 放置位置 |
| --- | --- | --- |
| 边端推流 | 平台为每个 `machineCode` 生成 | `cameraService.media.auth.token` |
| 浏览器播放 | Java 平台按登录用户临时签发 | 播放 URL query 或后端代理 |

推流 token 示例：

```text
rtsp://203.0.113.10:8554/GW0001/CAM001?token=edge_push_token_for_GW0001
```

播放 token 示例：

```text
https://platform.example.com/video/GW0001/CAM001/hls.m3u8?token=viewer_token
```

不要把现场摄像头 `sourceAuth.password` 下发给浏览器。

## 录像和截图

启动 MP4 录像：

```bash
curl 'http://127.0.0.1:8088/index/api/startRecord?secret=change_this_secret&type=1&vhost=__defaultVhost__&app=GW0001&stream=CAM001'
```

停止录像：

```bash
curl 'http://127.0.0.1:8088/index/api/stopRecord?secret=change_this_secret&type=1&vhost=__defaultVhost__&app=GW0001&stream=CAM001'
```

查询录像文件：

```bash
curl 'http://127.0.0.1:8088/index/api/getMp4RecordFile?secret=change_this_secret&vhost=__defaultVhost__&app=GW0001&stream=CAM001&period=2026-05'
```

截图：

```bash
curl -o snap.jpg 'http://127.0.0.1:8088/index/api/getSnap?secret=change_this_secret&url=rtsp://127.0.0.1:8554/GW0001/CAM001&timeout_sec=10&expire_sec=30'
```

## 生产检查清单

- ZLMediaKit `api.secret` 已替换，不使用默认值。
- `8554/tcp` 只允许边端或 VPN 访问。
- `/index/api/*` 不公网裸露。
- Nginx 已启用 HTTPS。
- `cameraService.media.auth.enabled=true`。
- `stream.path` 固定为 `<machineCode>/<cameraCode>`。
- Java 平台保存 `machineCode -> cameraCode -> token` 映射。
- ZLMediaKit `on_publish` 已校验路径和 token。
- ZLMediaKit `on_play` 已校验平台用户权限。
- 录像目录有磁盘配额和清理策略。
- WebRTC 端口和公网 NAT 配置已单独验证。

## 官方参考

- ZLMediaKit 官方文档：https://docs.zlmediakit.com/
- Guide / Docker 启动命令：https://docs.zlmediakit.com/guide/
- 播放 URL 规则：https://docs.zlmediakit.com/guide/media_server/play_url_rules.html
- 推流测试：https://docs.zlmediakit.com/guide/media_server/push_test.html
- RESTful API：https://docs.zlmediakit.com/guide/media_server/restful_api.html
- WebHook：https://docs.zlmediakit.com/guide/media_server/web_hook_api.html
