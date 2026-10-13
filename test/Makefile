CC ?= cc
# gnu11: the rc_* log macros use `, ##__VA_ARGS__`.
CFLAGS += -std=gnu11 -Wall -Wextra -Werror -pedantic
# Signed overflow usually wraps anyway, so a test of wrapping arithmetic
# passes on the broken code too. UBSan makes it abort instead.
CFLAGS += -fsanitize=undefined -fno-sanitize-recover=all
# A read one byte past a frame passes on the right values too. ASan stops it.
CFLAGS += -fsanitize=address

# src/ for the common private header, and every protocol folder, because the
# tests include a protocol's private header by plain name.
CPPFLAGS += -I../include -I../src $(addprefix -I,$(wildcard ../src/*/))

# The protocols' drivers touch the platform only through rc_priv.h, so they
# run once, on the ESP-IDF stubs, fed and read through a fake UART. The core
# and the bus layer run twice: through src/rc_port_esp.c against
# idf_stubs/ (esp/) and through src/rc_port_stm.c against hal_stubs/
# (stm/). The core cases live once, in core_cases.h. The *_proto tests touch
# nothing but the pure headers.

PROTOCOLS := -DCONFIG_RC_CRSF=1 -DCONFIG_RC_SBUS=1 -DCONFIG_RC_DR16=1
# What ESP-IDF passes: ESP_PLATFORM, and every protocol on.
ESP_CPPFLAGS := -Iidf_stubs -DESP_PLATFORM $(PROTOCOLS)
# What stm-forge passes: the family header the code includes, the HAL modules
# an app's hal_conf would enable, and every protocol option on.
STM_CPPFLAGS := -Ihal_stubs \
		-DCMSIS_device_header='"stm32_hal_stub.h"' \
		-DHAL_I2C_MODULE_ENABLED -DHAL_SPI_MODULE_ENABLED \
		-DHAL_UART_MODULE_ENABLED \
		$(PROTOCOLS) -DCONFIG_RC_DEBUG=1

# The core and the stubs every binary of a suite links.
CORE     := ../src/rc.c ../src/rc_log.c
ESP_CORE := $(CORE) ../src/rc_port_esp.c idf_stubs/idf_stubs.c
STM_CORE := $(CORE) ../src/rc_port_stm.c hal_stubs/hal_stubs.c

CRSF := ../src/crsf/crsf.c
SBUS := ../src/sbus/sbus.c
DR16 := ../src/dr16/dr16.c

PROTO_TESTS    := test_crsf_proto test_sbus_proto test_dr16_proto
PROTOCOL_TESTS := test_crsf test_sbus test_dr16
ESP_TESTS      := $(addprefix esp/,test_core test_port_esp)
STM_TESTS      := $(addprefix stm/,test_core test_port_stm)
TESTS          := $(PROTO_TESTS) $(PROTOCOL_TESTS) $(ESP_TESTS) $(STM_TESTS)

.PHONY: all clean test

all: $(TESTS)

test_crsf_proto: test_crsf_proto.c
test_sbus_proto: test_sbus_proto.c
test_dr16_proto: test_dr16_proto.c

test_crsf: test_crsf.c $(CRSF) $(ESP_CORE)
test_sbus: test_sbus.c $(SBUS) $(ESP_CORE)
test_dr16: test_dr16.c $(DR16) $(ESP_CORE)

esp/test_core: esp/test_core.c $(ESP_CORE)
esp/test_port_esp: esp/test_port_esp.c $(ESP_CORE)

stm/test_core: stm/test_core.c $(STM_CORE)
stm/test_port_stm: stm/test_port_stm.c $(STM_CORE)

$(PROTO_TESTS) $(PROTOCOL_TESTS) $(ESP_TESTS): CPPFLAGS += $(ESP_CPPFLAGS)
$(STM_TESTS): CPPFLAGS += $(STM_CPPFLAGS)

# Every printf() in a binary that links the HAL stubs also lands in
# stub_log_last, so a test can check what was logged.
$(STM_TESTS): LDFLAGS += -Wl,--wrap=printf

$(TESTS):
	$(CC) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) $(filter %.c,$^) -o $@ -lm

test: $(TESTS)
	@set -e; for t in $(TESTS); do echo "./$$t"; ./$$t; done

clean:
	rm -f $(TESTS)

# Headers are prerequisites so that editing one rebuilds. Without this, `make
# test` reruns a stale binary and reports green for code it never compiled.
# Recipes use $(filter %.c,$^) so the headers never reach the compiler.
HDRS  := $(wildcard ../include/*.h ../src/*.h ../src/*/*.h)
STUBS := $(wildcard idf_stubs/*.h idf_stubs/*/*.h hal_stubs/*.h)
$(TESTS): $(HDRS) $(STUBS) $(wildcard *_cases.h)
