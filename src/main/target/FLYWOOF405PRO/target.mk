F405_TARGETS   += $(TARGET)
FEATURES       += VCP ONBOARDFLASH

TARGET_SRC = \
drivers/accgyro/accgyro_spi_icm20689.c \
drivers/accgyro/accgyro_spi_icm426xx.c \
drivers/barometer/barometer_bmp280.c \
$(addprefix drivers/compass/,$(notdir $(wildcard $(SRC_DIR)/drivers/compass/*.c))) \
drivers/light_led.h \
drivers/light_ws2811strip.c \
drivers/max7456.c \

# notice - this file was programmatically generated and may be incomplete.

# This resource file generated using https://github.com/nerdCopter/target-convert
# Commit: d84474d + 1 file changed, 24 deletions(-)
