# OrderForge build
#
#   make                 debug build of the daemons, tools and spikes
#   make MODE=release    optimised, hardened build
#   make MODE=asan       AddressSanitizer build   (also: tsan, ubsan)
#   make test            build and run the unit tests
#   make spikes          build the phase-0 spikes
#   make deps            report which libraries were found
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

# ---------------------------------------------------------------- libraries
# Found with pkg-config; `sudo ./scripts/install-deps.sh` installs them.
HAVE_MYSQL  := $(shell pkg-config --exists mysqlclient 2>/dev/null && echo 1)
HAVE_SODIUM := $(shell pkg-config --exists libsodium 2>/dev/null && echo 1)
HAVE_FCGI   := $(shell (pkg-config --exists fcgi 2>/dev/null || test -f /usr/include/fcgiapp.h) && echo 1)
HAVE_ALL    := $(and $(HAVE_MYSQL),$(HAVE_SODIUM),$(HAVE_FCGI))

MYSQL_CFLAGS  := $(if $(HAVE_MYSQL),$(shell pkg-config --cflags mysqlclient))
MYSQL_LIBS    := $(if $(HAVE_MYSQL),$(shell pkg-config --libs mysqlclient))
SODIUM_CFLAGS := $(if $(HAVE_SODIUM),$(shell pkg-config --cflags libsodium))
SODIUM_LIBS   := $(if $(HAVE_SODIUM),$(shell pkg-config --libs libsodium))
FCGI_CFLAGS   := $(shell pkg-config --cflags fcgi 2>/dev/null)
FCGI_LIBS     := $(or $(shell pkg-config --libs fcgi 2>/dev/null),-lfcgi)

# ---------------------------------------------------------------- common library
COMMON_SRC := $(wildcard src/common/*.c)
COMMON_OBJ := $(COMMON_SRC:%.c=$(BUILD)/%.o)
LIBCOMMON  := $(BUILD)/libofcommon.a

# ---------------------------------------------------------------- store library (business logic)
STORE_SRC  := $(wildcard src/store/*.c)
STORE_OBJ  := $(STORE_SRC:%.c=$(BUILD)/%.o)
LIBSTORE   := $(BUILD)/libofstore.a
STORE_LIBS := $(LIBSTORE) $(LIBCOMMON) $(MYSQL_LIBS) $(SODIUM_LIBS)
$(STORE_OBJ): CFLAGS += $(MYSQL_CFLAGS) $(SODIUM_CFLAGS)
$(COMMON_OBJ): CFLAGS += $(SODIUM_CFLAGS)

# ---------------------------------------------------------------- daemons and tools
API_SRC := $(wildcard src/api/*.c)
API_OBJ := $(API_SRC:%.c=$(BUILD)/%.o)
CTL_SRC := $(wildcard src/ctl/*.c)
CTL_OBJ := $(CTL_SRC:%.c=$(BUILD)/%.o)
CORE_SRC := $(wildcard src/core/*.c)
CORE_OBJ := $(CORE_SRC:%.c=$(BUILD)/%.o)
BENCH_SRC := $(wildcard src/bench/*.c)
BENCH_OBJ := $(BENCH_SRC:%.c=$(BUILD)/%.o)
PAY_SRC := $(wildcard src/pay/*.c)
PAY_OBJ := $(PAY_SRC:%.c=$(BUILD)/%.o)
REPORT_SRC := $(wildcard src/report/*.c)
REPORT_OBJ := $(REPORT_SRC:%.c=$(BUILD)/%.o)
SUP_SRC := $(wildcard src/supervisor/*.c)
SUP_OBJ := $(SUP_SRC:%.c=$(BUILD)/%.o)
$(API_OBJ): CFLAGS += $(FCGI_CFLAGS)
$(CTL_OBJ) $(CORE_OBJ) $(BENCH_OBJ) $(REPORT_OBJ): CFLAGS += $(MYSQL_CFLAGS) $(SODIUM_CFLAGS)
$(PAY_OBJ): CFLAGS += $(SODIUM_CFLAGS)

BINS := $(BUILD)/of-supervisor $(BUILD)/of-core $(BUILD)/of-api $(BUILD)/of-pay \
        $(BUILD)/of-report $(BUILD)/of-ctl $(BUILD)/of-bench

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

.PHONY: all bins test spikes deps clean check-deps
all: check-deps bins spikes

check-deps:
ifneq ($(HAVE_ALL),1)
	@echo "error: missing libraries (mysqlclient: $(or $(HAVE_MYSQL),no), libsodium: $(or $(HAVE_SODIUM),no), fcgi: $(or $(HAVE_FCGI),no))" >&2
	@echo "       run: sudo ./scripts/install-deps.sh" >&2
	@false
endif

bins: $(BINS)

$(BUILD)/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@

$(LIBCOMMON): $(COMMON_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

$(LIBSTORE): $(STORE_OBJ)
	@mkdir -p $(dir $@)
	$(AR) rcs $@ $^

# of-api deliberately links no database or crypto library: it only frames requests.
$(BUILD)/of-api: $(API_OBJ) $(LIBCOMMON)
	$(CC) $(API_OBJ) -o $@ $(LDFLAGS) $(LIBCOMMON) $(FCGI_LIBS) $(LDLIBS)

$(BUILD)/of-core: $(CORE_OBJ) $(LIBSTORE) $(LIBCOMMON)
	$(CC) $(CORE_OBJ) -o $@ $(LDFLAGS) $(STORE_LIBS) $(LDLIBS)

# of-pay needs no database: only the message queues and libsodium for the HMAC.
$(BUILD)/of-pay: $(PAY_OBJ) $(LIBCOMMON)
	$(CC) $(PAY_OBJ) -o $@ $(LDFLAGS) $(LIBCOMMON) $(SODIUM_LIBS) $(LDLIBS)

# of-supervisor only forks, execs and waits: no database, no crypto.
$(BUILD)/of-supervisor: $(SUP_OBJ) $(LIBCOMMON)
	$(CC) $(SUP_OBJ) -o $@ $(LDFLAGS) $(LIBCOMMON) $(LDLIBS)

$(BUILD)/of-report: $(REPORT_OBJ) $(LIBSTORE) $(LIBCOMMON)
	$(CC) $(REPORT_OBJ) -o $@ $(LDFLAGS) $(STORE_LIBS) $(LDLIBS)

$(BUILD)/of-bench: $(BENCH_OBJ) $(LIBSTORE) $(LIBCOMMON)
	$(CC) $(BENCH_OBJ) -o $@ $(LDFLAGS) $(STORE_LIBS) $(LDLIBS)

$(BUILD)/of-ctl: $(CTL_OBJ) $(LIBSTORE) $(LIBCOMMON)
	$(CC) $(CTL_OBJ) -o $@ $(LDFLAGS) $(STORE_LIBS) $(LDLIBS)

# --- unit tests (no database needed)
$(BUILD)/tests/%: tests/unit/%.c $(LIBCOMMON)
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(SODIUM_CFLAGS) $< -o $@ $(LDFLAGS) $(LIBCOMMON) $(SODIUM_LIBS) $(LDLIBS)

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
	$(CC) $(CFLAGS) $(FCGI_CFLAGS) $< -o $@ $(LDFLAGS) $(LIBCOMMON) $(FCGI_LIBS) $(LDLIBS)

deps:
	@echo "mysqlclient : $(if $(HAVE_MYSQL),found,MISSING)"
	@echo "libsodium   : $(if $(HAVE_SODIUM),found,MISSING)"
	@echo "libfcgi     : $(if $(HAVE_FCGI),found,MISSING)"
	@echo "install all : sudo ./scripts/install-deps.sh"

clean:
	rm -rf build

-include $(COMMON_OBJ:.o=.d) $(STORE_OBJ:.o=.d) $(API_OBJ:.o=.d) $(CTL_OBJ:.o=.d) \
         $(CORE_OBJ:.o=.d) $(BENCH_OBJ:.o=.d) $(PAY_OBJ:.o=.d) $(REPORT_OBJ:.o=.d) \
         $(SUP_OBJ:.o=.d)
