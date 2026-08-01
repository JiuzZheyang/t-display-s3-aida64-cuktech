idf_component_register(SRCS "lcd_i80.c" "backlight.c" "button.c"
    INCLUDE_DIRS "."
    REQUIRES esp_lcd esp_driver_gpio esp_driver_ledc esp_timer freertos esp_hw_support)
