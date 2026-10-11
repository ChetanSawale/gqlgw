# gqlgw - GraphQL gateway for OpenResty
# C core: GNU C99, no dependencies.

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN     = -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter
ALLCFLAGS = -std=gnu99 $(WARN) $(CFLAGS) -fPIC -fvisibility=hidden
LDFLAGS ?=

ROOT     := $(CURDIR)
DEPS     := $(ROOT)/.deps
OPENRESTY:= $(DEPS)/openresty
NGINX    := $(OPENRESTY)/nginx/sbin/nginx
RESTY    := $(OPENRESTY)/bin/resty
B        := build

LIB_SRC  = src/util.c src/errors.c src/lexer.c src/parser.c src/json.c src/schema.c src/values.c \
           src/sdl.c src/gqlb.c src/routes.c src/validate.c src/plan.c src/introspect.c src/api.c
LIB_OBJ  = $(patsubst src/%.c,$(B)/obj/%.o,$(LIB_SRC))
HDRS     = $(wildcard src/*.h)

SAN      = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all lib clean test test-c test-t test-oracle fuzz deps schemas memcached-start memcached-stop bench

all: $(B)/libgql.so $(B)/gqlc

lib: $(B)/libgql.so

src/prelude.inc: src/prelude.graphqls scripts/embed_prelude.sh
	./scripts/embed_prelude.sh $< $@

$(B)/obj/sdl.o: src/prelude.inc

$(B)/obj/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(ALLCFLAGS) -c -o $@ $<

$(B)/libgql.so: $(LIB_OBJ)
	$(CC) -shared $(LDFLAGS) -o $@ $(LIB_OBJ) -lm

$(B)/libgql.a: $(LIB_OBJ)
	ar rcs $@ $(LIB_OBJ)

$(B)/gqlc: src/gqlc.c $(B)/libgql.a
	$(CC) $(ALLCFLAGS) -fvisibility=default -o $@ src/gqlc.c $(B)/libgql.a -lm

# ---- tests -----------------------------------------------------------------

# C unit tests, built with sanitizers
$(B)/test_c: tests/c/*.c tests/c/*.h $(LIB_SRC) $(HDRS)
	@mkdir -p $(B)
	$(CC) -std=gnu99 $(WARN) $(SAN) -Isrc -o $@ tests/c/*.c $(LIB_SRC) -lm

test-c: $(B)/test_c
	$(B)/test_c

# graphql-js differential tests (validation, introspection)
test-oracle: $(B)/gqlc
	node tests/oracle/run.mjs

# compile the SDL fixtures used by Test::Nginx
T_SCHEMAS = $(patsubst %.graphqls,%.gqlb,$(wildcard t/schemas/*.graphqls))
t/schemas/%.gqlb: t/schemas/%.graphqls $(B)/gqlc
	$(B)/gqlc compile -q -o $@ $<

schemas: $(T_SCHEMAS)

test-t: all schemas memcached-start
	PATH=$(OPENRESTY)/nginx/sbin:$(OPENRESTY)/bin:$$PATH \
	PERL5LIB=$(DEPS)/perl/lib/perl5 \
	TEST_NGINX_ROOT=$(ROOT) TEST_NGINX_MEMCACHED_PORT=21211 \
	prove -r t/

test: test-c test-oracle test-t

# ---- fuzzing (clang + libFuzzer) ---------------------------------------------

FUZZ_TIME ?= 60
$(B)/fuzz_%: tests/fuzz/fuzz_%.c $(LIB_SRC) $(HDRS)
	@mkdir -p $(B)
	clang -std=gnu99 -g -O1 -fsanitize=fuzzer,address,undefined -Isrc -o $@ $< $(LIB_SRC) -lm

$(B)/harness.gqlb: tests/oracle/harness.graphqls $(B)/gqlc
	$(B)/gqlc compile -q -o $@ -r $(B)/harness.routes.lua $<

fuzz: $(B)/fuzz_parse $(B)/fuzz_query $(B)/harness.gqlb
	@mkdir -p $(B)/corpus_parse $(B)/corpus_query $(B)/fuzz_artifacts
	$(B)/fuzz_parse tests/fuzz/regressions/*
	$(B)/fuzz_parse -max_total_time=$(FUZZ_TIME) -dict=tests/fuzz/graphql.dict -artifact_prefix=$(B)/fuzz_artifacts/parse- \
		$(B)/corpus_parse tests/corpus tests/fuzz/seeds
	GQL_FUZZ_SCHEMA=$(B)/harness.gqlb $(B)/fuzz_query -max_total_time=$(FUZZ_TIME) -max_len=4096 \
		-dict=tests/fuzz/graphql.dict -artifact_prefix=$(B)/fuzz_artifacts/query- $(B)/corpus_query tests/fuzz/seeds

# ---- deps / services ---------------------------------------------------------

deps:
	./scripts/deps.sh

memcached-start:
	@./scripts/memcached.sh start 21211

memcached-stop:
	@./scripts/memcached.sh stop 21211

bench: all
	./bench/run.sh

clean:
	rm -rf $(B) t/servroot* t/schemas/*.gqlb t/schemas/*.routes.lua
