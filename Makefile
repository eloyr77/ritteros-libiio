# libiio for RitterOS: libiio.a (libiio 0.26's interface, include/iio.h),
# the tools iio_info and iio_attr, and the test program iiotest, built with
# RitterOS's SDK (RITTEROS/sdk/CLAUDE.md).
#
#   make                                 build build/x86_64/: libiio.a, iio_info, iio_attr, iiotest
#   make ARCH=aarch64                    the same for 64-bit ARM
#   make install HOST=root@ADDRESS       copy the tools to /system/ARCH/bin there, iiotest to .../test
#   make check-host                      the library and iiotest on this Mac (ASan, UBSan)
#                                        against the pretend Pluto (tests/testiiod.py)
#   make check HOST=... IIOD_FROM=...    iiotest on RitterOS against the pretend Pluto running
#                                        here; IIOD_FROM is this Mac's address as the machine
#                                        sees it (10.0.2.2 from QEMU)
#
# Programs of your own link build/ARCH/libiio.a and take -I include.

# A RitterOS checkout: next to this project, or this project inside its projects/.
RITTEROS ?= $(patsubst %/sdk/ritteros.mk,%,$(firstword $(wildcard ../RitterOS/sdk/ritteros.mk ../../sdk/ritteros.mk)))
APP      := iio_info
KIND     := bin
SRCS     := tools/iio_info.c
CFLAGS   += -Werror -Iinclude
# (static: the programs carry the C library they use, so they also run on an
# installed system older than the RitterOS checkout; LINK=shared to share it)
LINK     ?= static

ifeq ($(wildcard $(RITTEROS)/sdk/ritteros.mk),)
$(error no RitterOS checkout found: git clone https://github.com/eloyr77/RitterOS.git next to this project, or make RITTEROS=/path/to/RitterOS)
endif
include $(RITTEROS)/sdk/ritteros.mk

LIB_SRCS := src/iio.c src/iioxml.c src/iionet.c
LIB_OBJS := $(patsubst %.c,$(OUT)/obj/%.o,$(LIB_SRCS))
LIBIIO   := $(OUT)/libiio.a
IIO_ATTR := $(OUT)/iio_attr
IIOTEST  := $(OUT)/iiotest

all: $(LIBIIO) $(IIO_ATTR) $(IIOTEST)

$(LIB_OBJS): src/iiopriv.h include/iio.h

$(LIBIIO): $(LIB_OBJS)
	@rm -f $@
	$(ZIG_EXE) ar rcs $@ $^

# iio_info is ritteros.mk's own program; the others are linked the same way.
$(PROGRAM): $(LIBIIO)
RITTEROS_LIBS := $(LIBIIO) $(RITTEROS_LIBS)

$(IIO_ATTR): $(OUT)/obj/tools/iio_attr.o $(LIBIIO) ritteros-sdk
	$(ZIG_EXE) ld.lld $(RITTEROS_LDFLAGS) -o $@ $(RITTEROS_START) $< $(RITTEROS_LIBS)

$(IIOTEST): $(OUT)/obj/tests/iiotest.o $(LIBIIO) ritteros-sdk
	$(ZIG_EXE) ld.lld $(RITTEROS_LDFLAGS) -o $@ $(RITTEROS_START) $< $(RITTEROS_LIBS)

.PHONY: install check check-host

# As ritteros.mk's deploy: a copy, then chmod (a new file is 0644).
install: $(PROGRAM) $(IIO_ATTR) $(IIOTEST)
	$(need_host)
	@for p in $(PROGRAM):bin $(IIO_ATTR):bin $(IIOTEST):test; do \
	    f=$${p%%:*}; d=/system/$(ARCH)/$${p##*:}/$$(basename $$f); \
	    $(SSH) $(HOST) "cat > $$d" < $$f && $(SSH) $(HOST) "chmod 755 $$d" || exit 1; \
	    echo "installed $$f as $(HOST):$$d"; \
	done

IIOD_PORT ?= 47791
IIOD_FROM ?= 10.0.2.2

check: install
	tests/check.sh ritteros "$(HOST)" "$(IIOD_FROM)" $(IIOD_PORT) $(ARCH) $(SSH_OPTS)

HOST_CC := cc -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter
build/host/iiotest: $(LIB_SRCS) tests/iiotest.c src/iiopriv.h include/iio.h tests/host/ritteros.h
	@mkdir -p $(dir $@)
	$(HOST_CC) -Itests/host -Iinclude -Isrc -o $@ tests/iiotest.c $(LIB_SRCS)

check-host: build/host/iiotest
	tests/check.sh host build/host/iiotest 127.0.0.1 $(IIOD_PORT)
