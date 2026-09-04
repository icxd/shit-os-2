# SPDX-License-Identifier: GPL-3.0-or-later
#
# Bootable image assembly: stage a GRUB tree, pack the initrd, burn an ISO.

find_program(GRUB_MKRESCUE NAMES grub-mkrescue grub2-mkrescue)
find_program(XORRISO xorriso)
find_program(QEMU NAMES qemu-system-x86_64)

set(ISO_ROOT "${CMAKE_BINARY_DIR}/isoroot")
set(ISO_PATH "${CMAKE_BINARY_DIR}/shit-os-2.iso")
set(INITRD_PATH "${CMAKE_BINARY_DIR}/initrd.tar")

# Everything that has to be built before the initrd can be packed: modules and
# userland programs register themselves here as they are declared.
get_property(SHITOS_IMAGE_TARGETS GLOBAL PROPERTY SHITOS_IMAGE_TARGETS)

# Everything under rootfs/ is copied into the image verbatim, so touching any
# of it has to repack. CONFIGURE_DEPENDS makes cmake re-glob when a file is
# added rather than silently shipping a stale archive.
file(GLOB_RECURSE SHITOS_ROOTFS_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/rootfs/*")

# The initrd is a plain ustar archive. `tar` is the tool; there is no bespoke
# image format to learn, and you can inspect a broken one with tar -tvf.
add_custom_command(
    OUTPUT "${INITRD_PATH}"
    COMMAND "${CMAKE_COMMAND}" -E env
            # Quote the whole assignment, not just the value: an unquoted
            # NAME="${VAR}" makes the quotes part of the value.
            "SHITOS_BUILD_DIR=${CMAKE_BINARY_DIR}"
            "${CMAKE_SOURCE_DIR}/tools/mkinitrd.sh" "${INITRD_PATH}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/mkinitrd.sh" ${SHITOS_IMAGE_TARGETS}
            ${SHITOS_ROOTFS_FILES}
    COMMENT "Packing initrd"
    VERBATIM
)
add_custom_target(initrd DEPENDS "${INITRD_PATH}")

if(GRUB_MKRESCUE AND XORRISO)
    add_custom_command(
        OUTPUT "${ISO_PATH}"
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${ISO_ROOT}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${ISO_ROOT}/boot/grub"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:kernel.elf>" "${ISO_ROOT}/boot/kernel.elf"
        COMMAND "${CMAKE_COMMAND}" -E copy "${INITRD_PATH}" "${ISO_ROOT}/boot/initrd.tar"
        COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_SOURCE_DIR}/grub/grub.cfg" "${ISO_ROOT}/boot/grub/grub.cfg"
        COMMAND "${GRUB_MKRESCUE}" -o "${ISO_PATH}" "${ISO_ROOT}"
        DEPENDS kernel.elf "${INITRD_PATH}" "${CMAKE_SOURCE_DIR}/grub/grub.cfg"
        COMMENT "Building ${ISO_PATH}"
        VERBATIM
    )
    add_custom_target(iso ALL DEPENDS "${ISO_PATH}")
else()
    add_custom_target(iso
        COMMAND "${CMAKE_COMMAND}" -E echo "grub-mkrescue and xorriso are required to build an ISO"
        COMMAND "${CMAKE_COMMAND}" -E false
    )
endif()

if(QEMU)
    add_custom_target(run
        COMMAND "${CMAKE_SOURCE_DIR}/tools/run-qemu.sh" "${ISO_PATH}"
        DEPENDS iso
        USES_TERMINAL
        VERBATIM
    )
    add_custom_target(run-headless
        COMMAND "${CMAKE_SOURCE_DIR}/tools/run-qemu.sh" --headless "${ISO_PATH}"
        DEPENDS iso
        USES_TERMINAL
        VERBATIM
    )
endif()
