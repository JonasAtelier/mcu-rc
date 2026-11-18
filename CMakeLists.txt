# Read by ESP-IDF (and so esp-forge) as a component, and by any other CMake
# project that add_subdirectory()s this repo, such as a CubeMX-generated STM32
# one. stm-forge never reads it: it compiles every .c under src/, protocol
# folders included, puts src/ on the include path of this library's sources
# and adds include/ -- which is why each port file, and every protocol
# driver, is empty when its platform or option is off. ESP_PLATFORM tells the
# two apart; IDF sets it in its early requirements pass as well as in the
# build.

if(NOT ESP_PLATFORM)
    # A static library `rc`. Every .c under src/, as stm-forge does: a
    # protocol file compiles to nothing unless its CONFIG_RC_<PROTOCOL> is 1,
    # so there is no source list here to keep in step with the IDF one below.
    file(GLOB_RECURSE rc_sources CONFIGURE_DEPENDS src/*.c)
    add_library(rc STATIC ${rc_sources})
    # include/ for the app; src/ only for this library's own sources, so a
    # protocol folder reaches rc_priv.h by plain name and no private header
    # is ever on the app's include path.
    target_include_directories(rc PUBLIC include PRIVATE src)

    # The options are the Kconfig bools, read from Kconfig so a new protocol
    # needs no line here: a true CMake variable CONFIG_RC_<PROTOCOL> (or
    # CONFIG_RC_DEBUG) becomes CONFIG_RC_<PROTOCOL>=1, what stm-forge passes.
    # Only bools: an int option cannot be 1-or-absent. Each `config` line in
    # Kconfig is followed directly by its type line, which is what the match
    # relies on. PUBLIC, because rc_log.h reads CONFIG_RC_DEBUG in the app's
    # sources.
    file(READ Kconfig rc_kconfig)
    string(REGEX MATCHALL "config RC_[A-Z0-9_]+[ \t\r\n]+bool" rc_options
           "${rc_kconfig}")
    list(TRANSFORM rc_options REPLACE "config (RC_[A-Z0-9_]+)[ \t\r\n]+bool"
         "CONFIG_\\1")
    set(rc_protocols_on "")
    foreach(option IN LISTS rc_options)
        if(${option})
            target_compile_definitions(rc PUBLIC ${option}=1)
            if(NOT option STREQUAL "CONFIG_RC_DEBUG")
                list(APPEND rc_protocols_on ${option})
            endif()
        endif()
    endforeach()
    if(NOT rc_protocols_on)
        message(WARNING "rc: no protocol is selected; "
                        "set(CONFIG_RC_<PROTOCOL> ON) before add_subdirectory()")
    endif()

    # The HAL is the app's. MCU_HAL_TARGET names a target carrying its include
    # dirs and defines (USE_HAL_DRIVER, the device, the hal_conf.h directory)
    # -- stm32cubemx in a CubeMX-generated project; MCU_DEVICE_HEADER is the
    # family header the public headers include as CMSIS_device_header, which
    # CubeMX does not define. Both are shared by every mcu-* library, so an
    # app sets them once. Left unset, the app provides them its own way.
    #
    # Only the target's include dirs and defines are copied, the target is
    # never linked: CubeMX up to 6.13 also hangs main.c, the HAL and the
    # startup file on stm32cubemx as INTERFACE sources, which linking would
    # compile into librc.a. A misspelt name fails at generate time.
    if(MCU_HAL_TARGET)
        target_include_directories(rc PRIVATE
            $<TARGET_PROPERTY:${MCU_HAL_TARGET},INTERFACE_INCLUDE_DIRECTORIES>)
        target_compile_definitions(rc PRIVATE
            $<TARGET_PROPERTY:${MCU_HAL_TARGET},INTERFACE_COMPILE_DEFINITIONS>)
    endif()
    if(MCU_DEVICE_HEADER)
        target_compile_definitions(rc PUBLIC
            "CMSIS_device_header=\"${MCU_DEVICE_HEADER}\"")
    endif()
    return()
endif()

# ESP-IDF: the protocol files are listed one by one, and PRIV_INCLUDE_DIRS
# "src" lets a protocol folder reach rc_priv.h by plain name.
#
# The core is always built, even with CONFIG_RC off; each protocol driver
# only when its Kconfig option is on. Kconfig values are available here
# during the build phase, and only SRCS depends on them -- never REQUIRES,
# which IDF resolves before sdkconfig. An unreferenced core costs nothing
# once --gc-sections has run.
#
# No file name here is one of esp-crsf-rc's (crsf.c, crsf_parse.h at the
# top of src/), and that repo is replaced by this one rather than linked
# next to it: both define crsf.h and crsf_init().
#
# REQUIRES driver, not esp_driver_*: the split components do not exist on
# IDF 5.2, and driver still pulls them in on later versions.
set(srcs "src/rc.c" "src/rc_log.c" "src/rc_port_esp.c")

if(CONFIG_RC AND NOT (CONFIG_RC_CRSF OR CONFIG_RC_SBUS OR CONFIG_RC_DR16))
    message(WARNING "rc: CONFIG_RC is on but no protocol is selected; "
                    "set CONFIG_RC_<PROTOCOL>=y (esp-forge: RC_<PROTOCOL> = y)")
endif()

if(CONFIG_RC_CRSF)
    list(APPEND srcs "src/crsf/crsf.c")
endif()
if(CONFIG_RC_SBUS)
    list(APPEND srcs "src/sbus/sbus.c")
endif()
if(CONFIG_RC_DR16)
    list(APPEND srcs "src/dr16/dr16.c")
endif()

idf_component_register(SRCS ${srcs}
                       INCLUDE_DIRS "include"
                       PRIV_INCLUDE_DIRS "src"
                       REQUIRES driver)
