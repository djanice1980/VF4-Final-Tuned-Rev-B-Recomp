# The lockstep tests: the translation against VF4debug's interpreter.
#
#   make -C tests/lockstep -j16 test
#
# Needs: the VF4debug checkout (VF4DEBUG), the decompilation checkout
# (VF4_DECOMP, for VF4debug's routine tables and the program image), the
# translator's output in work/gen/main (translator/translate.py), and gcc
# with 32-bit support (pacman: lib32-glibc lib32-gcc-libs), as VF4debug's
# own build needs. Nothing of either checkout is copied: everything builds
# under build/lockstep of this repository.

# Paths are relative (run from tests/lockstep, as make -C does): make cannot
# name a file with a space in its path ("Claude Data"), and the checkouts are
# reached through links in the build directory for the same reason.
VF4DEBUG ?= ../../../VF4debug
VF4_DECOMP ?= ../../../vf4ft-decomp
GEN ?= ../../work/gen/main
R := ../..
B := $(R)/build/lockstep
$(shell mkdir -p "$(B)/vf4debug" "$(B)/gen" "$(B)/main" "$(B)/rt" && \
        ln -sfn "$(abspath $(VF4DEBUG))" "$(B)/vd" && ln -sfn "$(abspath $(VF4_DECOMP))" "$(B)/dc" && \
        ln -sfn "$(abspath $(GEN))" "$(B)/gm")
VD := $(B)/vd
GM := $(B)/gm
DC := $(B)/dc

CC := gcc
# VF4debug's own flags (its Makefile), so its objects are what it tests itself with
VD_CFLAGS := -O2 -m32 -std=gnu11 -msse2 -mfpmath=sse -ffp-contract=off -fno-strict-aliasing \
             -Wall -Wextra -Wno-unused-parameter -I$(VD)/src -DDEFAULT_DECOMP='"$(DC)"'
# the recomp's code: the same floating-point model; warnings are errors
RT_CFLAGS := -O1 -m32 -std=gnu11 -msse2 -mfpmath=sse -ffp-contract=off -fno-strict-aliasing \
             -Wall -Wextra -Werror -I$(R)/runtime/cpu -I$(R)/tests/lockstep

VD_SRC := $(filter-out $(VD)/src/main.c,$(wildcard $(VD)/src/*.c))
VD_OBJ := $(patsubst $(VD)/src/%.c,$(B)/vf4debug/%.o,$(VD_SRC)) $(B)/vf4debug/tables.o
GEN_SRC := $(wildcard $(GM)/*.c)
GEN_OBJ := $(patsubst $(GM)/%.c,$(B)/main/%.o,$(GEN_SRC))
RT_OBJ := $(B)/rt/rt_core.o $(B)/rt/sh4rt_fpu.o $(B)/rt/insn_cases.o

all: $(B)/lockstep

$(B)/vf4debug/%.o: $(VD)/src/%.c
	$(CC) $(VD_CFLAGS) -c -o $@ $<

$(B)/gen/tables.c: $(VD)/tools/gen_tables.py
	python3 -I $(VD)/tools/gen_tables.py $(DC) $(B)/gen

$(B)/vf4debug/tables.o: $(B)/gen/tables.c
	$(CC) $(VD_CFLAGS) -O0 -g0 -c -o $@ $<

$(B)/main/%.o: $(GM)/%.c $(R)/runtime/cpu/sh4rt.h $(R)/tests/lockstep/lockstep_hooks.h
	$(CC) $(RT_CFLAGS) -include $(R)/tests/lockstep/lockstep_hooks.h -I$(GM) -c -o $@ $<

$(B)/rt/%.o: $(R)/runtime/cpu/%.c $(R)/runtime/cpu/sh4rt.h
	$(CC) $(RT_CFLAGS) -c -o $@ $<

$(B)/gen/insn_cases.c: $(R)/tests/lockstep/gen_cases.py $(R)/translator/sh4dec.py $(R)/translator/translate.py
	python3 -I $(R)/tests/lockstep/gen_cases.py $(B)/gen

$(B)/rt/insn_cases.o: $(B)/gen/insn_cases.c $(R)/runtime/cpu/sh4rt.h
	$(CC) $(RT_CFLAGS) -c -o $@ $<

$(B)/rt/lockstep.o: $(R)/tests/lockstep/lockstep.c $(R)/tests/lockstep/lockstep_cases.h
	$(CC) $(VD_CFLAGS) -Werror -I$(R)/tests/lockstep -c -o $@ $<

$(B)/lockstep: $(B)/rt/lockstep.o $(RT_OBJ) $(GEN_OBJ) $(VD_OBJ)
	$(CC) -m32 -o $@ $^ -lm

test: $(B)/lockstep
	VF4_IMAGE=$(DC)/work/main.bin $(B)/lockstep $(VD)/tests/pure_routines.txt $(R)/tests/lockstep/routines.txt

.PHONY: all test
