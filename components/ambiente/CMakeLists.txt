idf_component_register(SRCS "ambiente.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES dht esp_driver_gpio sensor_sync)