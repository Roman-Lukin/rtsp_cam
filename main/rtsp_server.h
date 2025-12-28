/**
 * @file rtsp_server.h
 * @brief Minimal RTSP/RTP server for H.264 video streaming
 * 
 * Supports up to 2 concurrent clients, UDP and TCP interleaved transport.
 */

#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief RTSP server configuration
 */
typedef struct {
    uint16_t rtsp_port;         /*!< RTSP server port (default 554) */
    uint16_t rtp_port_base;     /*!< Base port for RTP (default 5000) */
    uint16_t width;             /*!< Video width */
    uint16_t height;            /*!< Video height */
    uint8_t fps;                /*!< Frames per second */
    uint32_t bitrate;           /*!< Bitrate in bps (for SDP info) */
    uint8_t max_clients;        /*!< Maximum concurrent clients (max 2) */
} rtsp_server_config_t;

/**
 * @brief Default RTSP server configuration
 */
#define RTSP_SERVER_CONFIG_DEFAULT() { \
    .rtsp_port = 554,                  \
    .rtp_port_base = 5000,             \
    .width = 1920,                     \
    .height = 1080,                    \
    .fps = 25,                         \
    .bitrate = 2000000,                \
    .max_clients = 2,                  \
}

/**
 * @brief RTSP server handle
 */
typedef struct rtsp_server_t *rtsp_server_handle_t;

/**
 * @brief Initialize and start RTSP server
 * 
 * @param config Server configuration
 * @param handle Pointer to store server handle
 * @return ESP_OK on success
 */
esp_err_t rtsp_server_start(const rtsp_server_config_t *config, rtsp_server_handle_t *handle);

/**
 * @brief Stop RTSP server
 * 
 * @param handle Server handle
 * @return ESP_OK on success
 */
esp_err_t rtsp_server_stop(rtsp_server_handle_t handle);

/**
 * @brief Feed H.264 frame to RTSP server for streaming
 * 
 * This function should be called for each encoded H.264 frame.
 * The server will packetize and send to all connected clients.
 * 
 * @param handle Server handle
 * @param data H.264 NAL unit data (with start code 00 00 00 01 or 00 00 01)
 * @param size Data size in bytes
 * @param pts Presentation timestamp in microseconds
 * @param is_keyframe True if this is an IDR frame
 * @return ESP_OK on success
 */
esp_err_t rtsp_server_feed_frame(rtsp_server_handle_t handle, 
                                  const uint8_t *data, size_t size,
                                  int64_t pts, bool is_keyframe);

/**
 * @brief Get number of connected clients
 * 
 * @param handle Server handle
 * @return Number of clients currently streaming
 */
int rtsp_server_get_client_count(rtsp_server_handle_t handle);

#ifdef __cplusplus
}
#endif
