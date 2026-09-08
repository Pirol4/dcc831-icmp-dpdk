# SPDX-License-Identifier: BSD-3-Clause
# DCC831 - DPDK ICMP echo server
#
# Build with pkg-config against an installed DPDK, e.g. the one you built on
# the Cloudlab m510 node:
#
#   export PKG_CONFIG_PATH=$HOME/dpdk/build/meson-uninstalled
#   make
#
# or, for the classic (make-based) DPDK 20.08 build:
#
#   export PKG_CONFIG_PATH=$HOME/dpdk/build/lib/pkgconfig
#   make

APP = icmp-echo
SRCS-y := main.c

PKGCONF ?= pkg-config

ifeq ($(shell $(PKGCONF) --exists libdpdk && echo 0),0)

CFLAGS  += -O3 -Wall -Wextra $(shell $(PKGCONF) --cflags libdpdk)
LDFLAGS += $(shell $(PKGCONF) --libs libdpdk)

$(APP): $(SRCS-y) Makefile
	$(CC) $(CFLAGS) $(SRCS-y) -o $(APP) $(LDFLAGS)

.PHONY: clean
clean:
	rm -f $(APP)

else

# Fallback: DPDK 20.08 legacy build system.
# Set RTE_SDK to your dpdk/ checkout and RTE_TARGET to the built target.
ifeq ($(RTE_SDK),)
$(error "libdpdk not found via pkg-config. Set PKG_CONFIG_PATH, or set RTE_SDK to your dpdk/ directory for the legacy build.")
endif
RTE_TARGET ?= x86_64-native-linuxapp-gcc
include $(RTE_SDK)/mk/rte.vars.mk
CFLAGS += -O3 -Wall -Wextra
include $(RTE_SDK)/mk/rte.extapp.mk

endif
