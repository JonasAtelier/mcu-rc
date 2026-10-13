# ESP-IDF only. stm-forge never reads this file: it compiles every .c under
# src/, protocol folders included, puts src/ on the include path of this
# library's sources and adds include/ -- which is why each port file, and
# every protocol driver, is empty when its platform or option is off. Here
# the protocol files are listed one by one, and PRIV_INCLUDE_DIRS "src" lets
# a protocol folder reach rc_priv.h by plain name.
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
