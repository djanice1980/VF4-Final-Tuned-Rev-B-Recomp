# The two-PC game's security (runtime/host/secure.c, docs/design.md section
# 50) against CPace's test vectors and reference.py's values.
#
#   make -C tests/secure test
#
# Needs a C compiler and Python 3. Builds under build/secure of this
# repository. Paths are relative (run from tests/secure, as make -C does):
# make cannot name a file with a space in its path.
R := ../..
B := $(R)/build/secure
$(shell mkdir -p "$(B)")

CFLAGS := -O2 -std=gnu11 -Wall -Wextra -Werror -I$(R)/runtime/host -I$(R)/runtime/crypto -I$(B)
SRC := $(R)/tests/secure/secure_test.c $(R)/runtime/host/secure.c $(R)/runtime/crypto/monocypher.c \
       $(R)/runtime/crypto/monocypher-ed25519.c

all: $(B)/secure_test

$(B)/reference.h: $(R)/tests/secure/reference.py
	python3 -I $< $@

$(B)/secure_test: $(SRC) $(R)/runtime/host/secure.h $(B)/reference.h
	$(CC) $(CFLAGS) -o $@ $(SRC)

test: $(B)/secure_test
	$(B)/secure_test

.PHONY: all test
