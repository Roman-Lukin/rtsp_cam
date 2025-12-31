idf_component_register(SRCS "main.c" "network.c" "rtsp_server.c"
                            "lpr/lpr.c" "lpr/detection.c" "lpr/ocr.c"
                    INCLUDE_DIRS "." "lpr"
                    REQUIRES esp_h264 av_render codec_board media_lib_sal esp_eth nvs_flash esp_capture esp_video esp_video_codec esp_wifi esp_wifi_remote esp-dl)
