# boreale-link's host tests: `make test` builds and runs them with the
# sanitizers, the way CI does. The library itself is built by each homebrew
# from source/ (its own toolchain and flags).
CC      ?= cc
CFLAGS  ?= -std=gnu11 -Wall -Wextra -Wshadow -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
           -fno-sanitize-recover=undefined
OUT     := build/tests
INC     := -Isource -Itests

codec_SRC   := source/bl_json.c source/bl_kv.c source/mqtt_packet.c tests/codec/test.c
session_SRC := source/mqtt_packet.c source/mqtt_client.c source/bl_session.c tests/session/test.c
TESTS       := codec session

.PHONY: test clean
test: $(addprefix $(OUT)/,$(TESTS))
	@set -e; for t in $^; do $$t; done

$(OUT)/%: FORCE
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INC) $($*_SRC) -o $@

FORCE:

clean:
	rm -rf build
