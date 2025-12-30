idf_component_register(SRCS "main.c" "network.c" "rtsp_server.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_h264 av_render codec_board media_lib_sal esp_eth nvs_flash esp_capture esp_video esp_video_codec esp_wifi esp_wifi_remote)
