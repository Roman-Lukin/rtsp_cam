/**
 * @file rtsp_server.c
 * @brief Minimal RTSP/RTP server implementation for ESP32
 */

#include "rtsp_server.h"
#include "settings.h"

#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "RTSP";

// ============================================================================
// Constants and Defines
// ============================================================================

#define RTSP_MAX_CLIENTS        2
#define RTSP_BUFFER_SIZE        2048
#define RTP_MAX_PACKET_SIZE     1400    // MTU friendly
#define RTP_HEADER_SIZE         12
#define RTP_H264_PAYLOAD_TYPE   96

#define RTSP_TASK_STACK_SIZE    8192
#define RTSP_TASK_PRIORITY      10
#define TCP_INTERLEAVED_HEADER  4       // $ + channel + length(2)

// Pre-allocated buffer size for RTP packets (header + max payload + TCP header)
#define RTP_BUFFER_SIZE         (TCP_INTERLEAVED_HEADER + RTP_HEADER_SIZE + RTP_MAX_PACKET_SIZE)

// RTSP response templates
#define RTSP_RESPONSE_OK        "RTSP/1.0 200 OK\r\n"
#define RTSP_RESPONSE_NOT_FOUND "RTSP/1.0 404 Not Found\r\n"
#define RTSP_RESPONSE_ERROR     "RTSP/1.0 500 Internal Server Error\r\n"

// ============================================================================
// Types
// ============================================================================

typedef enum {
    CLIENT_STATE_INIT = 0,
    CLIENT_STATE_READY,
    CLIENT_STATE_PLAYING,
} rtsp_client_state_t;

typedef enum {
    TRANSPORT_UDP = 0,
    TRANSPORT_TCP_INTERLEAVED,
} rtsp_transport_t;

typedef struct {
    int socket;
    struct sockaddr_in addr;
    rtsp_client_state_t state;
    rtsp_transport_t transport;
    
    // Session info
    uint32_t session_id;
    
    // RTP info (UDP)
    int rtp_socket;
    int rtcp_socket;
    struct sockaddr_in rtp_addr;
    struct sockaddr_in rtcp_addr;
    uint16_t client_rtp_port;
    uint16_t client_rtcp_port;
    
    // TCP interleaved channel
    uint8_t rtp_channel;
    uint8_t rtcp_channel;
    
    // RTP state
    uint16_t rtp_seq;
    uint32_t rtp_timestamp;
    uint32_t ssrc;
    
    // Last activity
    int64_t last_activity;
    
    // Need keyframe flag (send SPS/PPS on next keyframe)
    bool need_keyframe;
    
    // Pre-allocated RTP buffer (optimization: avoid malloc in hot path)
    uint8_t rtp_buffer[RTP_BUFFER_SIZE];
} rtsp_client_t;

struct rtsp_server_t {
    rtsp_server_config_t config;
    
    int server_socket;
    bool running;
    
    rtsp_client_t clients[RTSP_MAX_CLIENTS];
    SemaphoreHandle_t clients_mutex;
    
    // SPS/PPS storage for SDP
    uint8_t *sps_data;
    size_t sps_size;
    uint8_t *pps_data;
    size_t pps_size;
    
    // IDR frame storage (for new clients)
    uint8_t *idr_data;
    size_t idr_size;
    uint32_t idr_ts;
    
    TaskHandle_t server_task;
    TaskHandle_t client_tasks[RTSP_MAX_CLIENTS];
};

// ============================================================================
// Forward declarations
// ============================================================================

static void rtsp_server_task(void *arg);
static void rtsp_client_task(void *arg);
static int handle_rtsp_request(rtsp_server_handle_t server, rtsp_client_t *client, 
                               char *request, char *response);
static int parse_rtsp_request(const char *request, char *method, char *uri, int *cseq);
static void send_rtp_h264(rtsp_server_handle_t server, rtsp_client_t *client,
                          const uint8_t *nal, size_t nal_size, uint32_t timestamp, bool marker);
static const uint8_t *find_nal_unit(const uint8_t *data, size_t size, size_t *nal_size, size_t *offset);

// ============================================================================
// Base64 encoding for SDP
// ============================================================================

static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static size_t base64_encode(const uint8_t *input, size_t input_len, char *output, size_t output_size)
{
    size_t i, j;
    for (i = 0, j = 0; i < input_len && j < output_size - 4;) {
        uint32_t octet_a = i < input_len ? input[i++] : 0;
        uint32_t octet_b = i < input_len ? input[i++] : 0;
        uint32_t octet_c = i < input_len ? input[i++] : 0;
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        
        output[j++] = base64_table[(triple >> 18) & 0x3F];
        output[j++] = base64_table[(triple >> 12) & 0x3F];
        output[j++] = (i > input_len + 1) ? '=' : base64_table[(triple >> 6) & 0x3F];
        output[j++] = (i > input_len) ? '=' : base64_table[triple & 0x3F];
    }
    output[j] = '\0';
    return j;
}

// ============================================================================
// NAL unit parsing (optimized with early exit)
// ============================================================================

static const uint8_t *find_nal_unit(const uint8_t *data, size_t size, size_t *nal_size, size_t *offset)
{
    size_t i = *offset;
    const size_t end = size - 3;
    
    // Find start code (00 00 01 or 00 00 00 01)
    while (i < end) {
        // Quick check for first zero byte - skip non-zero bytes fast
        if (data[i] != 0) {
            i++;
            continue;
        }
        
        if (data[i+1] == 0) {
            size_t nal_start;
            
            if (data[i+2] == 1) {
                // Found 00 00 01
                nal_start = i + 3;
            } else if (data[i+2] == 0 && i + 3 < size && data[i+3] == 1) {
                // Found 00 00 00 01
                nal_start = i + 4;
            } else {
                i++;
                continue;
            }
            
            // Find next start code or end
            size_t j = nal_start;
            const size_t search_end = size - 2;
            
            while (j < search_end) {
                if (data[j] == 0 && data[j+1] == 0) {
                    if (data[j+2] == 1 || (data[j+2] == 0 && j + 3 < size && data[j+3] == 1)) {
                        break;
                    }
                }
                j++;
            }
            
            if (j >= search_end) {
                j = size;  // NAL extends to end of buffer
            }
            
            *nal_size = j - nal_start;
            *offset = j;
            return &data[nal_start];
        }
        i++;
    }
    
    return NULL;
}

// ============================================================================
// RTP H.264 packetization (RFC 6184) - Optimized
// ============================================================================

// Inline RTP header construction for speed
static inline void build_rtp_header(uint8_t *packet, rtsp_client_t *client, 
                                     uint32_t timestamp, bool marker, uint8_t payload_type)
{
    packet[0] = 0x80;  // V=2, P=0, X=0, CC=0
    packet[1] = (marker ? 0x80 : 0x00) | (payload_type & 0x7F);
    packet[2] = (client->rtp_seq >> 8) & 0xFF;
    packet[3] = client->rtp_seq & 0xFF;
    packet[4] = (timestamp >> 24) & 0xFF;
    packet[5] = (timestamp >> 16) & 0xFF;
    packet[6] = (timestamp >> 8) & 0xFF;
    packet[7] = timestamp & 0xFF;
    packet[8] = (client->ssrc >> 24) & 0xFF;
    packet[9] = (client->ssrc >> 16) & 0xFF;
    packet[10] = (client->ssrc >> 8) & 0xFF;
    packet[11] = client->ssrc & 0xFF;
    
    client->rtp_seq++;
}

static void send_rtp_packet(rtsp_client_t *client, const uint8_t *data, size_t size, 
                            uint32_t timestamp, bool marker, uint8_t payload_type)
{
    // Use pre-allocated buffer instead of stack allocation
    uint8_t *packet = client->rtp_buffer;
    size_t packet_offset = 0;
    
    if (client->transport == TRANSPORT_TCP_INTERLEAVED) {
        // TCP Interleaved: $ + channel + length (2 bytes) + RTP packet
        size_t total_size = RTP_HEADER_SIZE + size;
        packet[0] = '$';
        packet[1] = client->rtp_channel;
        packet[2] = (total_size >> 8) & 0xFF;
        packet[3] = total_size & 0xFF;
        packet_offset = TCP_INTERLEAVED_HEADER;
    }
    
    // Build RTP header
    build_rtp_header(&packet[packet_offset], client, timestamp, marker, payload_type);
    
    // Copy payload
    memcpy(&packet[packet_offset + RTP_HEADER_SIZE], data, size);
    
    // Send in one syscall (reduces overhead)
    size_t total_len = packet_offset + RTP_HEADER_SIZE + size;
    
    if (client->transport == TRANSPORT_UDP) {
        sendto(client->rtp_socket, &packet[packet_offset], RTP_HEADER_SIZE + size, 0,
               (struct sockaddr *)&client->rtp_addr, sizeof(client->rtp_addr));
    } else {
        // Single send() instead of two - reduces syscall overhead
        send(client->socket, packet, total_len, MSG_NOSIGNAL);
    }
}

static void send_rtp_h264(rtsp_server_handle_t server, rtsp_client_t *client,
                          const uint8_t *nal, size_t nal_size, uint32_t timestamp, bool marker)
{
    if (nal_size == 0) return;
    
    // Single NAL unit mode (small NAL fits in one packet)
    if (nal_size <= RTP_MAX_PACKET_SIZE) {
        send_rtp_packet(client, nal, nal_size, timestamp, marker, RTP_H264_PAYLOAD_TYPE);
        return;
    }
    
    // FU-A fragmentation for large NALs
    uint8_t nal_type = nal[0] & 0x1F;
    uint8_t fu_indicator = (nal[0] & 0xE0) | 28;  // FU-A type = 28
    
    // Use client's pre-allocated buffer for FU packets
    uint8_t *packet = client->rtp_buffer;
    size_t packet_offset = (client->transport == TRANSPORT_TCP_INTERLEAVED) ? TCP_INTERLEAVED_HEADER : 0;
    uint8_t *fu_packet = &packet[packet_offset + RTP_HEADER_SIZE];
    fu_packet[0] = fu_indicator;
    
    size_t offset = 1;  // Skip NAL header
    const size_t max_chunk = RTP_MAX_PACKET_SIZE - 2;
    bool first = true;
    
    while (offset < nal_size) {
        size_t chunk_size = nal_size - offset;
        bool last = (chunk_size <= max_chunk);
        
        if (!last) {
            chunk_size = max_chunk;
        }
        
        // Build FU header
        uint8_t fu_header = nal_type & 0x1F;
        if (first) fu_header |= 0x80;  // S bit
        if (last) fu_header |= 0x40;   // E bit
        fu_packet[1] = fu_header;
        
        // Copy chunk directly into pre-allocated buffer
        memcpy(&fu_packet[2], &nal[offset], chunk_size);
        
        // Build and send packet
        size_t payload_size = chunk_size + 2;
        
        if (client->transport == TRANSPORT_TCP_INTERLEAVED) {
            size_t total_rtp = RTP_HEADER_SIZE + payload_size;
            packet[0] = '$';
            packet[1] = client->rtp_channel;
            packet[2] = (total_rtp >> 8) & 0xFF;
            packet[3] = total_rtp & 0xFF;
        }
        
        build_rtp_header(&packet[packet_offset], client, timestamp, 
                        last && marker, RTP_H264_PAYLOAD_TYPE);
        
        size_t total_len = packet_offset + RTP_HEADER_SIZE + payload_size;
        
        if (client->transport == TRANSPORT_UDP) {
            sendto(client->rtp_socket, &packet[packet_offset], RTP_HEADER_SIZE + payload_size, 0,
                   (struct sockaddr *)&client->rtp_addr, sizeof(client->rtp_addr));
        } else {
            send(client->socket, packet, total_len, MSG_NOSIGNAL);
        }
        
        offset += chunk_size;
        first = false;
    }
}

// ============================================================================
// RTSP Request Handlers
// ============================================================================

static int handle_options(rtsp_client_t *client, int cseq, char *response)
{
    return snprintf(response, RTSP_BUFFER_SIZE,
        RTSP_RESPONSE_OK
        "CSeq: %d\r\n"
        "Public: OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN\r\n"
        "\r\n", cseq);
}

static int handle_describe(rtsp_server_handle_t server, rtsp_client_t *client, 
                           const char *uri, int cseq, char *response)
{
    char sdp[1024];
    char sps_b64[256] = {0};
    char pps_b64[256] = {0};
    
    // Encode SPS/PPS to base64 if available
    if (server->sps_data && server->sps_size > 0) {
        base64_encode(server->sps_data, server->sps_size, sps_b64, sizeof(sps_b64));
    }
    if (server->pps_data && server->pps_size > 0) {
        base64_encode(server->pps_data, server->pps_size, pps_b64, sizeof(pps_b64));
    }
    
    // Get local IP
    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    getsockname(client->socket, (struct sockaddr *)&local_addr, &addr_len);
    char *local_ip = inet_ntoa(local_addr.sin_addr);
    
    int sdp_len = snprintf(sdp, sizeof(sdp),
        "v=0\r\n"
        "o=- %lu %lu IN IP4 %s\r\n"
        "s=ESP32 RTSP Stream\r\n"
        "c=IN IP4 0.0.0.0\r\n"
        "t=0 0\r\n"
        "m=video 0 RTP/AVP %d\r\n"
        "a=rtpmap:%d H264/90000\r\n"
        "a=fmtp:%d packetization-mode=1%s%s%s%s\r\n"
        "a=control:trackID=0\r\n",
        (unsigned long)esp_timer_get_time(), (unsigned long)esp_timer_get_time(),
        local_ip,
        RTP_H264_PAYLOAD_TYPE,
        RTP_H264_PAYLOAD_TYPE,
        RTP_H264_PAYLOAD_TYPE,
        sps_b64[0] ? "; sprop-parameter-sets=" : "",
        sps_b64,
        pps_b64[0] ? "," : "",
        pps_b64);
    
    return snprintf(response, RTSP_BUFFER_SIZE,
        RTSP_RESPONSE_OK
        "CSeq: %d\r\n"
        "Content-Type: application/sdp\r\n"
        "Content-Length: %d\r\n"
        "\r\n"
        "%s", cseq, sdp_len, sdp);
}

static int handle_setup(rtsp_server_handle_t server, rtsp_client_t *client,
                        const char *uri, int cseq, const char *request, char *response)
{
    // Parse Transport header
    const char *transport = strstr(request, "Transport:");
    if (!transport) {
        return snprintf(response, RTSP_BUFFER_SIZE,
            "RTSP/1.0 400 Bad Request\r\nCSeq: %d\r\n\r\n", cseq);
    }
    
    client->session_id = (uint32_t)(esp_timer_get_time() & 0xFFFFFFFF);
    client->ssrc = client->session_id;
    client->rtp_seq = 0;
    client->rtp_timestamp = 0;
    
    // Check for TCP interleaved
    if (strstr(transport, "RTP/AVP/TCP") || strstr(transport, "interleaved")) {
        client->transport = TRANSPORT_TCP_INTERLEAVED;
        client->rtp_channel = 0;
        client->rtcp_channel = 1;
        
        client->state = CLIENT_STATE_READY;
        client->need_keyframe = true;
        
        return snprintf(response, RTSP_BUFFER_SIZE,
            RTSP_RESPONSE_OK
            "CSeq: %d\r\n"
            "Session: %lu\r\n"
            "Transport: RTP/AVP/TCP;unicast;interleaved=0-1\r\n"
            "\r\n", cseq, (unsigned long)client->session_id);
    }
    
    // UDP transport
    client->transport = TRANSPORT_UDP;
    
    // Parse client_port
    const char *port_str = strstr(transport, "client_port=");
    if (port_str) {
        sscanf(port_str, "client_port=%hu-%hu", &client->client_rtp_port, &client->client_rtcp_port);
    } else {
        client->client_rtp_port = 6970;
        client->client_rtcp_port = 6971;
    }
    
    // Create RTP socket
    client->rtp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (client->rtp_socket < 0) {
        ESP_LOGE(TAG, "Failed to create RTP socket");
        return snprintf(response, RTSP_BUFFER_SIZE,
            RTSP_RESPONSE_ERROR "CSeq: %d\r\n\r\n", cseq);
    }
    
    // Set socket buffer size for lower latency
    int sndbuf = 65536;
    setsockopt(client->rtp_socket, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    
    // Setup client RTP address
    memcpy(&client->rtp_addr, &client->addr, sizeof(struct sockaddr_in));
    client->rtp_addr.sin_port = htons(client->client_rtp_port);
    
    // Find server port for this client
    int client_idx = (int)(client - server->clients);
    uint16_t server_rtp_port = server->config.rtp_port_base + client_idx * 2;
    uint16_t server_rtcp_port = server_rtp_port + 1;
    
    client->state = CLIENT_STATE_READY;
    client->need_keyframe = true;
    
    return snprintf(response, RTSP_BUFFER_SIZE,
        RTSP_RESPONSE_OK
        "CSeq: %d\r\n"
        "Session: %lu\r\n"
        "Transport: RTP/AVP;unicast;client_port=%d-%d;server_port=%d-%d\r\n"
        "\r\n", 
        cseq, (unsigned long)client->session_id,
        client->client_rtp_port, client->client_rtcp_port,
        server_rtp_port, server_rtcp_port);
}

static int handle_play(rtsp_server_handle_t server, rtsp_client_t *client,
                       int cseq, char *response)
{
    if (client->state != CLIENT_STATE_READY) {
        return snprintf(response, RTSP_BUFFER_SIZE,
            "RTSP/1.0 455 Method Not Valid in This State\r\nCSeq: %d\r\n\r\n", cseq);
    }
    
    client->state = CLIENT_STATE_PLAYING;
    client->need_keyframe = false;  // We'll send IDR now, no need to wait
    ESP_LOGI(TAG, "Client started playing, session=%lu", (unsigned long)client->session_id);
    
    // Send SPS/PPS/IDR immediately so decoder can start
    // Use the timestamp of the stored IDR so it matches the stream timeline
    uint32_t init_timestamp = server->idr_ts;
    
    if (server->sps_data && server->sps_size > 0) {
        send_rtp_h264(server, client, server->sps_data, server->sps_size, init_timestamp, false);
        ESP_LOGD(TAG, "Sent SPS to client (size=%d)", server->sps_size);
    }
    if (server->pps_data && server->pps_size > 0) {
        send_rtp_h264(server, client, server->pps_data, server->pps_size, init_timestamp, false);
        ESP_LOGD(TAG, "Sent PPS to client (size=%d)", server->pps_size);
    }
    if (server->idr_data && server->idr_size > 0) {
        send_rtp_h264(server, client, server->idr_data, server->idr_size, init_timestamp, true);
        ESP_LOGD(TAG, "Sent IDR to client (size=%d)", server->idr_size);
    }
    
    return snprintf(response, RTSP_BUFFER_SIZE,
        RTSP_RESPONSE_OK
        "CSeq: %d\r\n"
        "Session: %lu\r\n"
        "Range: npt=0.000-\r\n"
        "RTP-Info: url=trackID=0;seq=%d;rtptime=%lu\r\n"
        "\r\n",
        cseq, (unsigned long)client->session_id, client->rtp_seq, (unsigned long)client->rtp_timestamp);
}

static int handle_teardown(rtsp_client_t *client, int cseq, char *response)
{
    ESP_LOGI(TAG, "Client teardown, session=%lu", (unsigned long)client->session_id);
    client->state = CLIENT_STATE_INIT;
    
    if (client->rtp_socket > 0) {
        close(client->rtp_socket);
        client->rtp_socket = -1;
    }
    
    return snprintf(response, RTSP_BUFFER_SIZE,
        RTSP_RESPONSE_OK
        "CSeq: %d\r\n"
        "Session: %lu\r\n"
        "\r\n", cseq, (unsigned long)client->session_id);
}

static int parse_rtsp_request(const char *request, char *method, char *uri, int *cseq)
{
    // Parse first line: METHOD URI RTSP/1.0
    if (sscanf(request, "%s %s", method, uri) != 2) {
        return -1;
    }
    
    // Parse CSeq
    const char *cseq_str = strstr(request, "CSeq:");
    if (cseq_str) {
        sscanf(cseq_str, "CSeq: %d", cseq);
    } else {
        *cseq = 0;
    }
    
    return 0;
}

static int handle_rtsp_request(rtsp_server_handle_t server, rtsp_client_t *client,
                               char *request, char *response)
{
    char method[32] = {0};
    char uri[256] = {0};
    int cseq = 0;
    
    if (parse_rtsp_request(request, method, uri, &cseq) < 0) {
        return snprintf(response, RTSP_BUFFER_SIZE,
            "RTSP/1.0 400 Bad Request\r\n\r\n");
    }
    
    ESP_LOGD(TAG, "RTSP %s %s CSeq=%d", method, uri, cseq);
    
    if (strcmp(method, "OPTIONS") == 0) {
        return handle_options(client, cseq, response);
    } else if (strcmp(method, "DESCRIBE") == 0) {
        return handle_describe(server, client, uri, cseq, response);
    } else if (strcmp(method, "SETUP") == 0) {
        return handle_setup(server, client, uri, cseq, request, response);
    } else if (strcmp(method, "PLAY") == 0) {
        return handle_play(server, client, cseq, response);
    } else if (strcmp(method, "TEARDOWN") == 0) {
        return handle_teardown(client, cseq, response);
    } else if (strcmp(method, "GET_PARAMETER") == 0) {
        // Keep-alive
        return snprintf(response, RTSP_BUFFER_SIZE,
            RTSP_RESPONSE_OK "CSeq: %d\r\nSession: %lu\r\n\r\n", 
            cseq, (unsigned long)client->session_id);
    }
    
    return snprintf(response, RTSP_BUFFER_SIZE,
        "RTSP/1.0 501 Not Implemented\r\nCSeq: %d\r\n\r\n", cseq);
}

// ============================================================================
// Client task
// ============================================================================

typedef struct {
    rtsp_server_handle_t server;
    int client_idx;
} client_task_arg_t;

static void rtsp_client_task(void *arg)
{
    client_task_arg_t *task_arg = (client_task_arg_t *)arg;
    rtsp_server_handle_t server = task_arg->server;
    int client_idx = task_arg->client_idx;
    free(task_arg);
    
    rtsp_client_t *client = &server->clients[client_idx];
    char *buffer = malloc(RTSP_BUFFER_SIZE);
    char *response = malloc(RTSP_BUFFER_SIZE);
    
    if (!buffer || !response) {
        ESP_LOGE(TAG, "Failed to allocate client buffers");
        goto cleanup;
    }
    
    ESP_LOGI(TAG, "Client %d connected from %s", client_idx, inet_ntoa(client->addr.sin_addr));
    
    // Set socket timeout
    struct timeval tv = { .tv_sec = 60, .tv_usec = 0 };
    setsockopt(client->socket, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    
    while (server->running && client->socket > 0) {
        int len = recv(client->socket, buffer, RTSP_BUFFER_SIZE - 1, 0);
        
        if (len <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // Timeout - check if we should disconnect
                continue;
            }
            ESP_LOGI(TAG, "Client %d disconnected", client_idx);
            break;
        }
        
        buffer[len] = '\0';
        client->last_activity = esp_timer_get_time();
        
        // Handle RTSP request
        int resp_len = handle_rtsp_request(server, client, buffer, response);
        if (resp_len > 0) {
            send(client->socket, response, resp_len, 0);
        }
    }
    
cleanup:
    xSemaphoreTake(server->clients_mutex, portMAX_DELAY);
    
    if (client->socket > 0) {
        close(client->socket);
        client->socket = -1;
    }
    if (client->rtp_socket > 0) {
        close(client->rtp_socket);
        client->rtp_socket = -1;
    }
    client->state = CLIENT_STATE_INIT;
    server->client_tasks[client_idx] = NULL;
    
    xSemaphoreGive(server->clients_mutex);
    
    free(buffer);
    free(response);
    
    ESP_LOGI(TAG, "Client %d task ended", client_idx);
    vTaskDelete(NULL);
}

// ============================================================================
// Server task
// ============================================================================

static void rtsp_server_task(void *arg)
{
    rtsp_server_handle_t server = (rtsp_server_handle_t)arg;
    
    ESP_LOGI(TAG, "RTSP server started on port %d", server->config.rtsp_port);
    
    while (server->running) {
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_socket = accept(server->server_socket, 
                                   (struct sockaddr *)&client_addr, &addr_len);
        
        if (client_socket < 0) {
            if (server->running) {
                ESP_LOGE(TAG, "Accept failed: %d", errno);
            }
            continue;
        }
        
        ESP_LOGI(TAG, "Client connected from %s", inet_ntoa(client_addr.sin_addr));

        // Set TCP_NODELAY to reduce latency
        int flag = 1;
        setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        
        // Find free client slot
        xSemaphoreTake(server->clients_mutex, portMAX_DELAY);
        
        int slot = -1;
        for (int i = 0; i < server->config.max_clients; i++) {
            if (server->clients[i].socket <= 0) {
                slot = i;
                break;
            }
        }
        
        if (slot < 0) {
            ESP_LOGW(TAG, "No free client slots, rejecting connection");
            close(client_socket);
            xSemaphoreGive(server->clients_mutex);
            continue;
        }
        
        // Initialize client
        rtsp_client_t *client = &server->clients[slot];
        memset(client, 0, sizeof(rtsp_client_t));
        client->socket = client_socket;
        memcpy(&client->addr, &client_addr, sizeof(client_addr));
        client->state = CLIENT_STATE_INIT;
        client->rtp_socket = -1;
        client->last_activity = esp_timer_get_time();
        
        // Create client task
        client_task_arg_t *task_arg = malloc(sizeof(client_task_arg_t));
        task_arg->server = server;
        task_arg->client_idx = slot;
        
        char task_name[16];
        snprintf(task_name, sizeof(task_name), "rtsp_c%d", slot);
        
        if (xTaskCreate(rtsp_client_task, task_name, RTSP_TASK_STACK_SIZE, 
                        task_arg, RTSP_TASK_PRIORITY, &server->client_tasks[slot]) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create client task");
            close(client_socket);
            client->socket = -1;
            free(task_arg);
        }
        
        xSemaphoreGive(server->clients_mutex);
    }
    
    ESP_LOGI(TAG, "RTSP server task ended");
    vTaskDelete(NULL);
}

// ============================================================================
// Public API
// ============================================================================

esp_err_t rtsp_server_start(const rtsp_server_config_t *config, rtsp_server_handle_t *handle)
{
    if (!config || !handle) {
        return ESP_ERR_INVALID_ARG;
    }
    
    rtsp_server_handle_t server = calloc(1, sizeof(struct rtsp_server_t));
    if (!server) {
        return ESP_ERR_NO_MEM;
    }
    
    memcpy(&server->config, config, sizeof(rtsp_server_config_t));
    if (server->config.max_clients > RTSP_MAX_CLIENTS) {
        server->config.max_clients = RTSP_MAX_CLIENTS;
    }
    
    server->clients_mutex = xSemaphoreCreateMutex();
    if (!server->clients_mutex) {
        free(server);
        return ESP_ERR_NO_MEM;
    }
    
    // Initialize client slots
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        server->clients[i].socket = -1;
        server->clients[i].rtp_socket = -1;
    }
    
    // Create server socket
    server->server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server->server_socket < 0) {
        ESP_LOGE(TAG, "Failed to create socket");
        vSemaphoreDelete(server->clients_mutex);
        free(server);
        return ESP_FAIL;
    }
    
    int opt = 1;
    setsockopt(server->server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port = htons(config->rtsp_port),
    };
    
    if (bind(server->server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "Failed to bind socket");
        close(server->server_socket);
        vSemaphoreDelete(server->clients_mutex);
        free(server);
        return ESP_FAIL;
    }
    
    if (listen(server->server_socket, 4) < 0) {
        ESP_LOGE(TAG, "Failed to listen");
        close(server->server_socket);
        vSemaphoreDelete(server->clients_mutex);
        free(server);
        return ESP_FAIL;
    }
    
    server->running = true;
    
    // Start server task
    if (xTaskCreate(rtsp_server_task, "rtsp_srv", RTSP_TASK_STACK_SIZE,
                    server, RTSP_TASK_PRIORITY, &server->server_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create server task");
        close(server->server_socket);
        vSemaphoreDelete(server->clients_mutex);
        free(server);
        return ESP_FAIL;
    }
    
    *handle = server;
    ESP_LOGI(TAG, "RTSP server initialized");
    return ESP_OK;
}

esp_err_t rtsp_server_stop(rtsp_server_handle_t handle)
{
    if (!handle) return ESP_ERR_INVALID_ARG;
    
    handle->running = false;
    
    // Close server socket to unblock accept()
    if (handle->server_socket > 0) {
        close(handle->server_socket);
        handle->server_socket = -1;
    }
    
    // Close all client connections
    xSemaphoreTake(handle->clients_mutex, portMAX_DELAY);
    for (int i = 0; i < RTSP_MAX_CLIENTS; i++) {
        if (handle->clients[i].socket > 0) {
            close(handle->clients[i].socket);
            handle->clients[i].socket = -1;
        }
        if (handle->clients[i].rtp_socket > 0) {
            close(handle->clients[i].rtp_socket);
            handle->clients[i].rtp_socket = -1;
        }
    }
    xSemaphoreGive(handle->clients_mutex);
    
    // Wait for tasks to end
    vTaskDelay(pdMS_TO_TICKS(100));
    
    vSemaphoreDelete(handle->clients_mutex);
    free(handle->sps_data);
    free(handle->pps_data);
    free(handle->idr_data);
    free(handle);
    
    ESP_LOGI(TAG, "RTSP server stopped");
    return ESP_OK;
}

esp_err_t rtsp_server_feed_frame(rtsp_server_handle_t handle, 
                                  const uint8_t *data, size_t size,
                                  int64_t pts, bool is_keyframe)
{
    if (!handle || !data || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Convert PTS to RTP timestamp (90kHz clock)
    // PTS is in milliseconds
    uint32_t rtp_timestamp = (uint32_t)(pts * 90);
    
    // Parse NAL units
    size_t offset = 0;
    size_t nal_size;
    const uint8_t *nal;
    
    while ((nal = find_nal_unit(data, size, &nal_size, &offset)) != NULL) {
        if (nal_size == 0) continue;
        
        uint8_t nal_type = nal[0] & 0x1F;
        
        // Store SPS/PPS for SDP
        if (nal_type == 7 && (!handle->sps_data || handle->sps_size != nal_size)) {
            free(handle->sps_data);
            handle->sps_data = malloc(nal_size);
            if (handle->sps_data) {
                memcpy(handle->sps_data, nal, nal_size);
                handle->sps_size = nal_size;
            }
        } else if (nal_type == 8 && (!handle->pps_data || handle->pps_size != nal_size)) {
            free(handle->pps_data);
            handle->pps_data = malloc(nal_size);
            if (handle->pps_data) {
                memcpy(handle->pps_data, nal, nal_size);
                handle->pps_size = nal_size;
            }
        } else if (nal_type == 5) {
            // Store IDR frame for new clients
            // Always update to keep the most recent IDR
            if (handle->idr_data) free(handle->idr_data);
            
            handle->idr_data = malloc(nal_size);
            if (handle->idr_data) {
                memcpy(handle->idr_data, nal, nal_size);
                handle->idr_size = nal_size;
                handle->idr_ts = rtp_timestamp;
                ESP_LOGD(TAG, "Stored IDR frame (size=%d)", nal_size);
            }
        }
        
        // Check if this is the last NAL in the frame (for marker bit)
        size_t peek_offset = offset;
        size_t peek_size;
        bool is_last_nal = (find_nal_unit(data, size, &peek_size, &peek_offset) == NULL);
        
        // Send to all playing clients
        xSemaphoreTake(handle->clients_mutex, portMAX_DELAY);
        
        for (int i = 0; i < handle->config.max_clients; i++) {
            rtsp_client_t *client = &handle->clients[i];
            
            if (client->state != CLIENT_STATE_PLAYING) continue;
            
            // SPS/PPS/IDR are sent on PLAY command, stream immediately
            send_rtp_h264(handle, client, nal, nal_size, rtp_timestamp, is_last_nal);
        }
        
        xSemaphoreGive(handle->clients_mutex);
    }
    
    return ESP_OK;
}

int rtsp_server_get_client_count(rtsp_server_handle_t handle)
{
    if (!handle) return 0;
    
    int count = 0;
    xSemaphoreTake(handle->clients_mutex, portMAX_DELAY);
    
    for (int i = 0; i < handle->config.max_clients; i++) {
        if (handle->clients[i].state == CLIENT_STATE_PLAYING) {
            count++;
        }
    }
    
    xSemaphoreGive(handle->clients_mutex);
    return count;
}
