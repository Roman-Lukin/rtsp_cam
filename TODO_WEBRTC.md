# WebRTC Integration TODO

## Architektura

```
┌─────────────────────────────────────────────────────────────────┐
│                        Browser                                   │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │  webrtc_preview.html                                     │   │
│  │  - SSE EventSource (signaling receive)                   │   │
│  │  - POST requests (signaling send)                        │   │
│  │  - RTCPeerConnection                                     │   │
│  │  - <video> element (H.264 decode)                        │   │
│  └─────────────────────────────────────────────────────────┘   │
└───────────────────────────┬─────────────────────────────────────┘
                            │ HTTPS + SSE
                            ▼
┌─────────────────────────────────────────────────────────────────┐
│                       ESP32-P4                                   │
│                                                                  │
│  ┌─────────────────┐     ┌──────────────────────────────────┐  │
│  │ webrtc_         │────▶│ esp_peer_signaling               │  │
│  │ signaling.c     │     │ - SSE (GET /webrtc/signal)       │  │
│  │ (HTTPS:443)     │     │ - POST /webrtc/signal/post       │  │
│  └─────────────────┘     └──────────────────────────────────┘  │
│           │                          │                          │
│           ▼                          ▼                          │
│  ┌─────────────────┐     ┌──────────────────────────────────┐  │
│  │ webrtc_stream.c │────▶│ esp_webrtc                       │  │
│  │ start/stop      │     │ - ICE/DTLS/SRTP                  │  │
│  └─────────────────┘     │ - RTP packetization              │  │
│           │              └──────────────────────────────────┘  │
│           ▼                          │                          │
│  ┌─────────────────┐                 │                          │
│  │ main.c          │◀────────────────┘                          │
│  │ - esp_capture   │                                            │
│  │ - H.264 encoder │                                            │
│  └─────────────────┘                                            │
└─────────────────────────────────────────────────────────────────┘
```

## Referencja

Przykład: `esp-webrtc-solution/solutions/doorbell_local/`

---

## TODO Lista

### 1. [ ] Dodać komponenty WebRTC do projektu

W pliku `main/idf_component.yml` dodać zależności:
- `esp_webrtc` (z esp-webrtc-solution/components/)
- `esp_peer` (z esp-webrtc-solution/components/)
- `webrtc_utils` (z esp-webrtc-solution/components/)
- `esp_websocket_client`
- `nghttp`

Można użyć `override_path` lub skopiować komponenty do projektu.

```yaml
# Przykład z override_path
tempotian/esp_webrtc:
    override_path: ../../esp-webrtc-solution/components/esp_webrtc
tempotian/esp_peer:
    override_path: ../../esp-webrtc-solution/components/esp_peer
webrtc_utils:
    override_path: ../../esp-webrtc-solution/components/webrtc_utils
```

---

### 2. [ ] Skopiować/utworzyć certyfikaty HTTPS

WebRTC wymaga HTTPS (`getUserMedia` w przeglądarce wymaga secure context).

Utworzyć folder `main/certs/` z:
- `servercert.pem` (self-signed)
- `prvtkey.pem` (klucz prywatny)

**Opcja A:** Skopiować z `doorbell_local/main/certs/`

**Opcja B:** Wygenerować nowe:
```bash
openssl req -x509 -newkey rsa:2048 -keyout prvtkey.pem -out servercert.pem -days 365 -nodes
```

---

### 3. [ ] Utworzyć webrtc_signaling.c/h

Skopiować i dostosować `webrtc_http_server.c` z doorbell_local.

Kluczowe funkcje:
- `esp_signaling_get_http_impl()` - implementacja signaling
- SSE endpoint: `GET /webrtc/signal`
- POST endpoint: `POST /webrtc/signal/post`
- Test page: `GET /webrtc/test`

Użyć `httpd_ssl_start()` zamiast `httpd_start()`.

Kluczowe elementy:
```c
// SSE dla signaling (server → browser)
static esp_err_t webrtc_signal_get_handler(httpd_req_t *req) {
    httpd_resp_set_type(req, "text/event-stream");
    // ...
}

// POST dla signaling (browser → server)
static esp_err_t webrtc_signal_post_handler(httpd_req_t *req) {
    // Parse JSON: offer, answer, candidate, bye
    // ...
}
```

---

### 4. [ ] Utworzyć webrtc_stream.c/h

Moduł zarządzający WebRTC session. Na bazie `webrtc.c` z doorbell_local.

Funkcje:
- `start_webrtc_stream()`
- `stop_webrtc_stream()`
- `webrtc_event_handler()`

Konfiguracja:
```c
esp_webrtc_cfg_t cfg = {
    .peer_cfg = {
        .video_info = {
            .codec = ESP_PEER_VIDEO_CODEC_H264,
            .width = 1280,  // lub 1920
            .height = 720,  // lub 1080
            .fps = 25,
        },
        .video_dir = ESP_PEER_MEDIA_DIR_SEND_ONLY,
        // audio opcjonalnie
    },
    .signaling_impl = esp_signaling_get_http_impl(),
    .peer_impl = esp_peer_get_default_impl(),
};
```

---

### 5. [ ] Zintegrować z istniejącym esp_capture

W `main.c` mamy już `capture_handle` i H.264 encoding.

WebRTC potrzebuje `esp_webrtc_media_provider_t` z:
- `capture`: `esp_capture_handle_t`
- `player`: `av_render_handle_t` (opcjonalny dla receive)

```c
esp_webrtc_media_provider_t media_provider = {
    .capture = capture_handle,  // istniejący
    .player = NULL,             // nie odbieramy video
};
esp_webrtc_set_media_provider(webrtc, &media_provider);
```

**Uwaga:** Może być potrzebny dual sink lub shared sink dla RTSP + WebRTC.

---

### 6. [ ] Utworzyć stronę webrtc_preview.html

Na bazie `webrtc_test.html` z doorbell_local.

Uproszczona wersja bez doorbell features:
- Video preview (remote stream)
- Connect/Disconnect button
- Status indicator

Osadzić przez `EMBED_TXTFILES` w `CMakeLists.txt`.

Kluczowe elementy JS:
```javascript
// SSE dla signaling
eventSource = new EventSource('/webrtc/signal');
eventSource.onmessage = async (event) => {
    const message = JSON.parse(event.data);
    // handle offer, answer, candidate
};

// Wysyłanie do ESP
function sendSignalingMessage(msg) {
    fetch('/webrtc/signal/post', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(msg)
    });
}

// RTCPeerConnection
peerConnection = new RTCPeerConnection(configuration);
peerConnection.ontrack = event => {
    remoteVideo.srcObject = event.streams[0];
};
```

---

### 7. [ ] Zmodyfikować CMakeLists.txt

Dodać do `main/CMakeLists.txt`:

```cmake
set(srcs 
    "main.c"
    "http_server.c"
    "network.c"
    "rtsp_server.c"
    "ov5647_helper.c"
    "webrtc_signaling.c"   # NOWE
    "webrtc_stream.c"      # NOWE
)

idf_component_register(SRCS ${srcs}
    EMBED_TXTFILES 
        "web/index.html"
        "web/webrtc_preview.html"           # NOWE
        "certs/servercert.pem"              # NOWE
        "certs/prvtkey.pem"                 # NOWE
    INCLUDE_DIRS "."
)
```

---

### 8. [ ] Zaktualizować sdkconfig.defaults

Dodać konfigurację dla WebRTC:

```ini
# HTTPS support
CONFIG_HTTPD_SSL_SUPPORT=y
CONFIG_ESP_TLS_SERVER=y

# mbedTLS
CONFIG_MBEDTLS_SSL_MAX_CONTENT_LEN=16384
CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN=y

# Stack sizes (jeśli potrzeba)
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192
```

---

### 9. [ ] Połączyć z HTTP server settings

Opcje integracji:

**Opcja A: Osobny HTTPS server dla WebRTC (port 443)**
- HTTP settings na porcie 80
- HTTPS WebRTC na porcie 443
- Prostsze, ale dwa serwery

**Opcja B: Jeden serwer HTTPS**
- Migracja całego http_server.c na HTTPS
- Jeden serwer, bardziej spójne

Dodać link do `/webrtc/test` w `index.html`:
```html
<a href="/webrtc/test">🎬 Live Preview (WebRTC)</a>
```

---

### 10. [ ] Rozwiązać konflikt RTSP vs WebRTC

Oba używają tego samego H.264 capture sink.

Opcje:
1. **Dual sink** - jeden dla RTSP, jeden dla WebRTC
2. **Shared sink** z dwoma consumers
3. **Przełączanie** między trybami (RTSP/WebRTC)

WebRTC używa `esp_webrtc_set_media_provider()` które może współdzielić capture.

```c
// Dual sink approach
esp_capture_sink_handle_t rtsp_sink, webrtc_sink;
esp_capture_sink_setup(capture, 0, &sink_cfg, &rtsp_sink);
esp_capture_sink_setup(capture, 1, &sink_cfg, &webrtc_sink);
```

---

### 11. [ ] Testowanie i debugowanie

1. Build i flash
2. Test połączenia HTTPS: `https://<IP>/webrtc/test`
3. Test SSE signaling (sprawdzić w DevTools → Network → EventStream)
4. Test WebRTC connection
5. Weryfikacja video stream w przeglądarce
6. Test równoczesny z RTSP

**Debug tips:**
- Monitor: logi `WEBRTC`, `PEER`, `SIGNALING`
- Browser DevTools: Console + Network
- `chrome://webrtc-internals/` dla szczegółów WebRTC

---

### 12. [ ] Optymalizacja i cleanup

Po działającym prototypie:
- Usunąć niepotrzebny kod doorbell (ring, open_door, music)
- Zoptymalizować użycie pamięci
- Dodać graceful shutdown
- Dokumentacja

---

## Przydatne linki

- [esp-webrtc-solution](https://github.com/espressif/esp-webrtc-solution)
- [WebRTC API MDN](https://developer.mozilla.org/en-US/docs/Web/API/WebRTC_API)
- [doorbell_local example](../esp-webrtc-solution/solutions/doorbell_local/)

---

## Status

| Task | Status | Notes |
|------|--------|-------|
| Komponenty WebRTC | ⬜ | |
| Certyfikaty HTTPS | ⬜ | |
| webrtc_signaling.c | ⬜ | |
| webrtc_stream.c | ⬜ | |
| Integracja capture | ⬜ | |
| webrtc_preview.html | ⬜ | |
| CMakeLists.txt | ⬜ | |
| sdkconfig.defaults | ⬜ | |
| HTTP server integration | ⬜ | |
| RTSP/WebRTC conflict | ⬜ | |
| Testing | ⬜ | |
| Cleanup | ⬜ | |
