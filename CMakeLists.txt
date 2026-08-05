cmake_minimum_required(VERSION 3.13)

option(KVM_DEBUG "Enable debug defines and softer optimisation" ON)

if(KVM_DEBUG)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
else()
    set(CMAKE_BUILD_TYPE MinSizeRel CACHE STRING "Build type" FORCE)
endif()

set(PICO_BOARD "pico" CACHE STRING "Pico board type")
set(PICO_SDK_FETCH_FROM_GIT OFF)
set(PIO_USB_DP_PIN_DEFAULT 14 CACHE STRING "Default USB D+ Pin Number")

if(NOT PICO_SDK_PATH AND DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif()
if(NOT PICO_SDK_PATH)
    get_filename_component(PICO_SDK_PATH
        "${CMAKE_CURRENT_LIST_DIR}/../deskhop/pico-sdk" ABSOLUTE)
endif()
if(NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR
        "Pico SDK not found at '${PICO_SDK_PATH}'. "
        "Set PICO_SDK_PATH or keep the deskhop tree next to kvm.")
endif()

include(${PICO_SDK_PATH}/pico_sdk_import.cmake)

project(kvm_minimal C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

set(PICO_PIO_USB_DIR ${CMAKE_CURRENT_LIST_DIR}/../deskhop/Pico-PIO-USB)
if(NOT EXISTS "${PICO_PIO_USB_DIR}/src/pio_usb.h")
    message(FATAL_ERROR "Pico-PIO-USB not found at ${PICO_PIO_USB_DIR}")
endif()

add_library(Pico-PIO-USB STATIC
    ${PICO_PIO_USB_DIR}/src/pio_usb.c
    ${PICO_PIO_USB_DIR}/src/pio_usb_host.c
    ${PICO_PIO_USB_DIR}/src/usb_crc.c
)
pico_generate_pio_header(Pico-PIO-USB ${PICO_PIO_USB_DIR}/src/usb_tx.pio)
pico_generate_pio_header(Pico-PIO-USB ${PICO_PIO_USB_DIR}/src/usb_rx.pio)
target_link_libraries(Pico-PIO-USB PRIVATE
    pico_stdlib
    pico_multicore
    hardware_pio
    hardware_dma
)
target_include_directories(Pico-PIO-USB PUBLIC ${PICO_PIO_USB_DIR}/src)

set(KVM_C_WARNINGS
    -Wall
    -Wextra
    -Werror=return-type
    -Wno-unused-parameter
)

set(KVM_COMMON_SOURCES
    ${CMAKE_CURRENT_LIST_DIR}/src/main.c
    ${CMAKE_CURRENT_LIST_DIR}/src/board.c
    ${CMAKE_CURRENT_LIST_DIR}/src/usb_device.c
    ${CMAKE_CURRENT_LIST_DIR}/src/usb_host.c
    ${CMAKE_CURRENT_LIST_DIR}/src/keyboard.c
    ${CMAKE_CURRENT_LIST_DIR}/src/router.c
    ${CMAKE_CURRENT_LIST_DIR}/src/protocol.c
    ${CMAKE_CURRENT_LIST_DIR}/src/uart.c
    ${PICO_TINYUSB_PATH}/src/portable/raspberrypi/pio_usb/hcd_pio_usb.c
)

set(KVM_COMMON_INCLUDES
    ${CMAKE_CURRENT_LIST_DIR}/src
    ${PICO_PIO_USB_DIR}/src
)

function(kvm_add_firmware TARGET_NAME BOARD_ROLE)
    add_executable(${TARGET_NAME} ${KVM_COMMON_SOURCES})

    target_include_directories(${TARGET_NAME} PRIVATE ${KVM_COMMON_INCLUDES})

    target_compile_definitions(${TARGET_NAME} PRIVATE
        BOARD_ROLE=${BOARD_ROLE}
        PICO_BOARD_ROLE=${BOARD_ROLE}
        PIO_USB_USE_TINYUSB=1
        PIO_USB_DP_PIN_DEFAULT=${PIO_USB_DP_PIN_DEFAULT}
    )

    if(KVM_DEBUG)
        target_compile_definitions(${TARGET_NAME} PRIVATE KVM_DEBUG=1)
    endif()

    target_compile_options(${TARGET_NAME} PRIVATE ${KVM_C_WARNINGS})

    target_link_libraries(${TARGET_NAME} PRIVATE
        pico_stdlib
        pico_unique_id
        pico_multicore
        hardware_gpio
        hardware_uart
        hardware_pio
        hardware_dma
        tinyusb_device
        tinyusb_host
        Pico-PIO-USB
    )

    pico_enable_stdio_usb(${TARGET_NAME} 0)
    pico_enable_stdio_uart(${TARGET_NAME} 0)

    pico_add_extra_outputs(${TARGET_NAME})

    set_target_properties(${TARGET_NAME} PROPERTIES
        OUTPUT_NAME ${TARGET_NAME}
    )
endfunction()

kvm_add_firmware(deskhop-a 0)
kvm_add_firmware(deskhop-b 1)

message(STATUS "KVM role targets: deskhop-a (ROLE_A), deskhop-b (ROLE_B)")
message(STATUS "KVM_DEBUG=${KVM_DEBUG}")
message(STATUS "PICO_SDK_PATH=${PICO_SDK_PATH}")
