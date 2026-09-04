# SPDX-License-Identifier: GPL-3.0-or-later
#
# add_shitos_module(<name> SOURCES <files...>)
#
# Produces <name>.ko: an ELF64 relocatable object the kernel loads at runtime.
# Modules are built with the same freestanding flags as the kernel and with
# -mcmodel=kernel, because they are mapped into the module window 256 MiB above
# the kernel image, where 32-bit displacements still reach.

function(add_shitos_module MODULE_NAME)
    cmake_parse_arguments(ARG "" "" "SOURCES" ${ARGN})

    set(objects_target "${MODULE_NAME}_objects")
    add_library(${objects_target} OBJECT ${ARG_SOURCES})
    target_link_libraries(${objects_target} PRIVATE shitos_kernel_flags)
    target_include_directories(${objects_target} PRIVATE "${CMAKE_SOURCE_DIR}")
    target_compile_options(${objects_target} PRIVATE
        $<$<COMPILE_LANGUAGE:C,CXX>:-mcmodel=kernel>
        $<$<COMPILE_LANGUAGE:C,CXX>:-fno-omit-frame-pointer>
    )

    set(ko_path "${CMAKE_CURRENT_BINARY_DIR}/${MODULE_NAME}.ko")

    # -r keeps the output relocatable, which is what the loader expects. A
    # single-source module would already be one, but linking explicitly means
    # multi-file modules work the same way.
    add_custom_command(
        OUTPUT "${ko_path}"
        COMMAND "${SHITOS_LLD}" -r -o "${ko_path}" $<TARGET_OBJECTS:${objects_target}>
        # Depend on the object files, not just the target that builds them.
        # Naming the target alone makes ninja build the objects but judge the
        # .ko up to date against nothing, so editing a driver -- or a header it
        # uses -- silently ships the previous build.
        DEPENDS $<TARGET_OBJECTS:${objects_target}> ${objects_target}
        COMMENT "Linking module ${MODULE_NAME}.ko"
        COMMAND_EXPAND_LISTS
        VERBATIM
    )

    add_custom_target(${MODULE_NAME} ALL DEPENDS "${ko_path}")
    set_property(GLOBAL APPEND PROPERTY SHITOS_IMAGE_TARGETS ${MODULE_NAME})
    # And the file, so the initrd repacks when the module changes rather than
    # only when the list of modules does.
    set_property(GLOBAL APPEND PROPERTY SHITOS_IMAGE_FILES "${ko_path}")
endfunction()
