idf_component_register(
        SRCS "main.c"
        INCLUDE_DIRS "."
        PRIV_REQUIRES bt driver esp_hid nvs_flash
)
