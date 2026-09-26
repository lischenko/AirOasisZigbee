# Hardware-independent protocol/mapping test; requires only a hosted C compiler.
CC ?= cc
CFLAGS ?= -std=c17 -Wall -Wextra -Werror -I../ncs/src

.PHONY: test clean
test: build/test_aoia_protocol
	./build/test_aoia_protocol

build/test_aoia_protocol: test_aoia_protocol.c ../ncs/src/aoia_protocol.c ../ncs/src/aoia_protocol.h
	mkdir -p build
	$(CC) $(CFLAGS) test_aoia_protocol.c ../ncs/src/aoia_protocol.c -o $@

clean:
	rm -rf build
