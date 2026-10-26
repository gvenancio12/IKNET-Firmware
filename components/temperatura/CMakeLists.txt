idf_component_register(SRCS "temperatura.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio
                       PRIV_REQUIRES ds18b20 onewire_bus)
