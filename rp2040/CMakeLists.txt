# machboot supervisor: RP2040 firmware for Sergei and Mozart (docs/rp2040.md)
#
#   cmake -B build -DPICO_SDK_PATH=/path/to/pico-sdk   (1.5.x)
#   make -C build
#   -> build/machboot-sv.uf2: hold BOOT, apply power, copy it to RPI-RP2

cmake_minimum_required(VERSION 3.13)
include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(machboot_sv C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(machboot-sv
	src/main.c
	src/djtag.c
	src/jtag_rp2040.c
	src/usb_descriptors.c
	src/xlink.c
	src/xlink_rp2040.c
	src/msc.c
	src/msc_tusb.c
	src/ecp5.c
	src/store.c
	src/cache.c
	src/sv.c
	src/sv_rp2040.c
)

# The flash size: the pico-sdk's default for PICO_BOARD=pico is 2 MB, and
# its flash_range_erase() halts the chip (hard_assert) on anything past
# that. Sergei and Mozart have a W25Q32 (4 MB); the supervisor reads the
# chip's own size at start (src/sv_rp2040.c) and keeps within it, so the
# SDK's limit is set to the most any RP2040 can have.
target_compile_definitions(machboot-sv PRIVATE PICO_FLASH_SIZE_BYTES=0x1000000)

# Sergei's and Mozart's crystal takes longer to start than the Pico's: the
# SDK's default wait (1 ms) makes the RP2040's start less reliable there.
target_compile_definitions(machboot-sv PRIVATE PICO_XOSC_STARTUP_DELAY_MULTIPLIER=64)
target_include_directories(machboot-sv PRIVATE src)
target_compile_options(machboot-sv PRIVATE -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(machboot-sv pico_stdlib pico_unique_id hardware_uart hardware_clocks hardware_spi hardware_flash hardware_sync
	tinyusb_device)
pico_set_program_name(machboot-sv "machboot supervisor")
pico_enable_stdio_usb(machboot-sv 0)
pico_enable_stdio_uart(machboot-sv 0)
pico_add_extra_outputs(machboot-sv)
