# Fuzzing the network stack on the host (thesis plan WP2). How it works: common.h.
#
#   make -C fuzz              libFuzzer binaries in build/libfuzzer/ (needs clang + libclang-rt-dev)
#   make -C fuzz run T=http   libFuzzer on one target for FUZZ_TIME seconds (default 600),
#                             new inputs in work/T, crashes in findings/
#   make -C fuzz minimize T=http   add the work/T inputs that increase coverage to corpus/T
#   make -C fuzz check        replay corpus/* with gcc + ASan/UBSan (part of the top-level make check)
#   make -C fuzz mutate       plain random mutation with gcc + ASan/UBSan, MUTATE_RUNS per target
#   make -C fuzz coverage     line coverage of the stack by corpus/* (gcc + gcov)
#   make -C fuzz seeds        regenerate the seed inputs (from the check_net.sh captures + hand-written)
#
# Targets: net (raw frames, both ports), wan (raw frames, WAN port only), http, tcp,
# dhcp_server, dhcp_client, rip.

TARGETS     = net wan http tcp dhcp_server dhcp_client rip
ROOT        = ..
STACK_SRC   = $(wildcard $(ROOT)/kernel/net/*.c) $(ROOT)/kernel/random.c $(ROOT)/kernel/cmdline.c $(ROOT)/libk/util/strbuf.c
INCLUDES    = -Iinclude -I$(ROOT)/libk/include -I$(ROOT)/kernel/compat/i386/include
BASE_CFLAGS = -g -O1 -fno-omit-frame-pointer -fno-pie -fno-common -D__LF $(INCLUDES) -Wall -Wno-unused-function -MMD -MP
FUZZ_TIME  ?= 600
MUTATE_RUNS ?= 20000

V ?= replay
OUT = build/$(V)
ifeq ($(V),libfuzzer)
  CC       = clang
  SAN_CC   = -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=undefined \
             -fno-sanitize-address-globals-dead-stripping
  SAN_LD   = -fsanitize=fuzzer,address,undefined
  DRIVER   =
else ifeq ($(V),cov)
  CC       = gcc
  SAN_CC   = --coverage
  SAN_LD   = --coverage
  DRIVER   = $(OUT)/drivers.o
else ifeq ($(V),mutate)
  CC       = gcc
  SAN_CC   = -fsanitize=address,undefined -fno-sanitize-recover=undefined
  SAN_LD   = $(SAN_CC)
  DRIVER   = $(OUT)/drivers.o
  DRIVER_FLAGS = -DFUZZ_MUTATOR
else
  CC       = gcc
  SAN_CC   = -fsanitize=address,undefined -fno-sanitize-recover=undefined
  SAN_LD   = $(SAN_CC)
  DRIVER   = $(OUT)/drivers.o
endif
CFLAGS = $(BASE_CFLAGS) $(SAN_CC)

STACK_OBJ = $(patsubst %.c,$(OUT)/stack/%.o,$(notdir $(STACK_SRC)))
HARNESS   = $(OUT)/common.o $(OUT)/stubs.o $(OUT)/www.o $(DRIVER)
BINS      = $(addprefix $(OUT)/fuzz_,$(TARGETS))
vpath %.c $(sort $(dir $(STACK_SRC)))

.PHONY: all bins check mutate coverage run seeds clean
all:
	$(MAKE) V=libfuzzer bins

bins: $(BINS)

# The stack's globals go into their own sections, which the harness snapshots
# after boot and restores after every input
$(OUT)/stack/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $(@:.o=.raw.o)
	objcopy --rename-section .data=fuzz_state_data --rename-section .bss=fuzz_state_bss $(@:.o=.raw.o) $@
	@if objdump -h $@ | awk '{print $$2}' | grep -qE '^\.(data|bss)'; then \
	    echo "$@: globals outside .data/.bss would escape the per-input reset"; rm -f $@; exit 1; fi

$(OUT)/fuzz_wan.o: fuzz_net.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -DFUZZ_WAN_ONLY -c $< -o $@

$(OUT)/drivers.o: drivers.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(DRIVER_FLAGS) -c $< -o $@

$(OUT)/%.o: %.c
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) -c $< -o $@

# The real dashboard, so responses are as large as on the router
$(OUT)/www.o: $(ROOT)/www/index.html
	@mkdir -p $(OUT)/www
	gzip -9 -n -c $< > $(OUT)/www/dashboard.html.gz
	cd $(OUT)/www && ld -r -b binary -z noexecstack dashboard.html.gz -o ../www.o

$(BINS): $(OUT)/fuzz_%: $(OUT)/fuzz_%.o $(HARNESS) $(STACK_OBJ)
	$(CC) $(SAN_LD) -no-pie $^ -o $@

check:
	$(MAKE) V=replay bins
	@for t in $(TARGETS); do build/replay/fuzz_$$t corpus/$$t || exit 1; done

mutate:
	$(MAKE) V=mutate bins
	@for t in $(TARGETS); do (cd build/mutate && ./fuzz_$$t $(MUTATE_RUNS) ../../corpus/$$t) || exit 1; done

coverage:
	rm -f build/cov/stack/*.gcda
	$(MAKE) V=cov bins
	@for t in $(TARGETS); do build/cov/fuzz_$$t --fork corpus/$$t > /dev/null || exit 1; done
	python3 coverage.py build/cov/stack $(STACK_SRC)

# New inputs go to work/T (not committed); corpus/T is read as seeds
run:
	$(MAKE) V=libfuzzer bins
	@mkdir -p work/$(T) findings
	build/libfuzzer/fuzz_$(T) -max_total_time=$(FUZZ_TIME) -max_len=8192 -timeout=10 -print_final_stats=1 \
	    -artifact_prefix=findings/$(T)- work/$(T) corpus/$(T)

# Add to corpus/T only those work/T inputs that add coverage (libFuzzer's merge)
minimize:
	build/libfuzzer/fuzz_$(T) -merge=1 -max_len=8192 corpus/$(T) work/$(T)
	@echo "corpus/$(T): $$(ls corpus/$(T) | wc -l) inputs"

seeds:
	python3 seeds.py

clean:
	rm -rf build

-include $(wildcard $(OUT)/*.d $(OUT)/stack/*.d)
