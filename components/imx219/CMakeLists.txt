idf_component_register(SRCS "imx219.c"
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "private_include"
                       REQUIRES esp_cam_sensor driver)

# Force linking of the detect function for primary I2C address (0x10)
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u imx219_detect")
