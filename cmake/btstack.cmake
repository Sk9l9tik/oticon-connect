# BTstack as a static library: LE central + L2CAP CoC over libusb, POSIX run loop.
# Only what asha_probe needs; config/btstack_config.h selects the features.

set(BTSTACK ${CMAKE_SOURCE_DIR}/third_partry/btstack)

pkg_check_modules(LIBUSB REQUIRED IMPORTED_TARGET libusb-1.0)

add_library(btstack STATIC
    # core
    ${BTSTACK}/src/ad_parser.c
    ${BTSTACK}/src/btstack_crypto.c
    ${BTSTACK}/src/btstack_linked_list.c
    ${BTSTACK}/src/btstack_memory.c
    ${BTSTACK}/src/btstack_memory_pool.c
    ${BTSTACK}/src/btstack_run_loop.c
    ${BTSTACK}/src/btstack_tlv.c
    ${BTSTACK}/src/btstack_util.c
    ${BTSTACK}/src/hci.c
    ${BTSTACK}/src/hci_cmd.c
    ${BTSTACK}/src/hci_dump.c
    ${BTSTACK}/src/hci_event.c
    ${BTSTACK}/src/hci_event_builder.c
    ${BTSTACK}/src/l2cap.c
    ${BTSTACK}/src/l2cap_signaling.c
    # LE: GATT client, Security Manager, bonds in TLV
    ${BTSTACK}/src/ble/att_dispatch.c
    ${BTSTACK}/src/ble/gatt_client.c
    ${BTSTACK}/src/ble/le_device_db_tlv.c
    ${BTSTACK}/src/ble/sm.c
    # platform: POSIX run loop / TLV / HCI dump, libusb transport, Realtek dongle
    ${BTSTACK}/platform/posix/btstack_run_loop_posix.c
    ${BTSTACK}/platform/posix/btstack_tlv_posix.c
    ${BTSTACK}/platform/posix/hci_dump_posix_fs.c
    ${BTSTACK}/platform/libusb/hci_transport_h2_libusb.c
    ${BTSTACK}/chipset/realtek/btstack_chipset_realtek.c
    # crypto for SM: ECDH (LE Secure Connections), AES-128 (also our RPA check)
    ${BTSTACK}/3rd-party/micro-ecc/uECC.c
    ${BTSTACK}/3rd-party/rijndael/rijndael.c
)
# SYSTEM: warnings from BTstack headers don't show up in our code
target_include_directories(btstack SYSTEM PUBLIC
    ${CMAKE_SOURCE_DIR}/config
    ${BTSTACK}/src
    ${BTSTACK}/src/ble
    ${BTSTACK}/platform/posix
    ${BTSTACK}/platform/embedded
    ${BTSTACK}/platform/libusb
    ${BTSTACK}/chipset/realtek
    ${BTSTACK}/3rd-party/micro-ecc
    ${BTSTACK}/3rd-party/rijndael
)
target_link_libraries(btstack PUBLIC PkgConfig::LIBUSB Threads::Threads m)
