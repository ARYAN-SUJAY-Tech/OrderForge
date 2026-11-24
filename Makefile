# OrderForge build
#
#   make                 debug build of everything that can be built
#   make MODE=release    optimised, hardened build
#   make MODE=asan       AddressSanitizer build   (also: tsan, ubsan)
#   make test            build and run the unit tests
#   make spikes          build the phase-0 spikes
#   make deps            report which optional libraries were found
#   make clean

MODE  ?= debug
BUILD := build/$(MODE)
CC    ?= gcc

# ---------------------------------------------------------------- flags
CSTD   := -std=c11 -D_GNU_SOURCE
WARN   := -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes \
          -Wmissing-prototypes -Wformat=2 -Wvla -Wpointer-arith
HARDEN := -fstack-protector-strong -fPIE
LDHARD := -pie -Wl,-z,relro,-z,now

ifeq ($(MODE),debug)
  OPT := -O0 -g3
else ifeq ($(MODE),release)
  # Ubuntu's gcc already defines _FORTIFY_SOURCE=3; pin it to 2 as the plan states.
  OPT := -O2 -g -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=2
else ifeq ($(MODE),asan)
  OPT := -O1 -g -fsanitize=address -fno-omit-frame-pointer
else ifeq ($(MODE),tsan)
  OPT := -O1 -g -fsanitize=thread
  # TSan cannot map its shadow memory under the high ASLR entropy of recent kernels
  # ("unexpected memory mapping"), so tsan binaries run with ASLR disabled.
  RUN := setarch -R
else ifeq ($(MODE),ubsan)
  OPT := -O1 -g -fsanitize=undefined -fno-sanitize-recover=all
else
  $(error unknown MODE '$(MODE)': use debug, release, asan, tsan or ubsan)
endif

SANFLAGS := $(filter -fsanitize=%,$(OPT))
CFLAGS   += $(CSTD) $(WARN) $(HARDEN) $(OPT) -Isrc -MMD -MP
LDFLAGS  += $(LDHARD) $(SANFLAGS)
LDLIBS   += -pthread

# ---------------------------------------------------------------- optional libraries
HAVE_MYSQL  := $(shell pkg-config --exists mysqlclient 2>/dev/null && echo 1)
HAVE_SODIUM := $(shell pkg-config --exists libsodium 2>/dev/null && echo 1)
HAVE_FCGI   := $(shell test -f /usr/include/fcgiapp.h && echo 1)

MYSQL_CFLAGS  := $(if $(HAVE_MYSQL),$(shell pkg-config --cflags mysqlclient))
MYSQL_LIBS    := $(if $(HAVE_MYSQL),$(shell pkg-config --libs mysqlclient))
SODIUM_CFLAGS := $(if $(HAVE_SODIUM),$(shell pkg-config --cflags libsodium))
SODIUM_LIBS   := $(if $(HAVE_SODIUM),$(shell pkg-config --libs libsodium))
FCGI_LIBS     := -lfcgi

# ---------------------------------------------------------------- common library
COMMON_SRC := $(wildcard src/common/*.c)
COMMON_OBJ := $(COMMON_SRC:%.c=$(BUILD)/%.o)
LIBCOMMON  := $(BUILD)/libofcommon.a

# ---------------------------------------------------------------- tests
TEST_SRC := $(wildcard tests/unit/test_*.c)
TEST_BIN := $(TEST_SRC:tests/unit/%.c=$(BUILD)/tests/%)

# ---------------------------------------------------------------- spikes
SPIKES := $(BUILD)/spikes/shm_counter
ifeq ($(HAVE_MYSQL),1)
  SPIKES += $(BUILD)/spikes/mysql_stmt
endif
ifeq ($(HAVE_FCGI),1)
  SPIKES += $(BUILD)/spikes/fcgi_hello
endif

.PHONY: all test spikes deps clean
all: $(LIBCOMMON) spikes

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBCOMMON): $(COMMON_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# --- tests
$(BUILD)/tests/%: tests/unit/%.c $(LIBCOMMON)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LIBCOMMON) $(LDLIBS)

test: $(TEST_BIN)
	@pass=0; fail=0; \
	for t in $(TEST_BIN); do \
	  if $(RUN) $$t; then pass=$$((pass+1)); else fail=$$((fail+1)); echo "FAILED: $$t"; fi; \
	done; \
	echo "unit tests: $$pass passed, $$fail failed"; test $$fail -eq 0

# --- spikes
spikes: $(SPIKES)
ifneq ($(HAVE_MYSQL),1)
	@echo "note: libmysqlclient-dev not found, spike mysql_stmt skipped (see 'make deps')"
endif
ifneq ($(HAVE_FCGI),1)
	@echo "note: libfcgi-dev not found, spike fcgi_hello skipped (see 'make deps')"
endif

$(BUILD)/spikes/shm_counter: spikes/shm_counter.c $(LIBCOMMON)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LIBCOMMON) $(LDLIBS)

$(BUILD)/spikes/mysql_stmt: spikes/mysql_stmt.c $(LIBCOMMON)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(MYSQL_CFLAGS) $< -o $@ $(LDFLAGS) $(LIBCOMMON) $(MYSQL_LIBS) $(LDLIBS)

$(BUILD)/spikes/fcgi_hello: spikes/fcgi_hello.c $(LIBCOMMON)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS) $(LIBCOMMON) $(FCGI_LIBS) $(LDLIBS)

deps:
	@echo "mysqlclient : $(if $(HAVE_MYSQL),found,MISSING)"
	@echo "libsodium   : $(if $(HAVE_SODIUM),found,MISSING)"
	@echo "libfcgi     : $(if $(HAVE_FCGI),found,MISSING)"
	@echo "install all : sudo ./scripts/install-deps.sh"

clean:
	rm -rf build

-include $(COMMON_OBJ:.o=.d)
