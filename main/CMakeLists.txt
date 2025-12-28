idf_component_register(SRCS "main.c" "network.c"
                    INCLUDE_DIRS "."
                    REQUIRES esp_h264 av_render codec_board media_lib_sal esp_eth)
