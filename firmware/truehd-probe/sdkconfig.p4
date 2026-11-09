# The pre-production P4 on the owner's boards: revision v1.3, 360 MHz and not 400, as the sink's overlay
# (firmware/hearth-sink/sdkconfig.p4) found the hard way; and its flash mode, which the sink's
# bootloader sets.
CONFIG_IDF_TARGET="esp32p4"
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y
CONFIG_PM_SLEEP_CLK_ICG_ENABLE=n
CONFIG_ESPTOOLPY_FLASHMODE_QIO=y
