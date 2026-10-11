# gqlgw - GraphQL gateway for OpenResty
# C core: GNU C99, no dependencies.

CC      ?= cc
CFLAGS  ?= -O2 -g
WARN     = -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wno-unused-parameter
ALLCFLAGS = -std=gnu99 $(WARN) $(CFLAGS) -fPIC -fvisibility=hidden
LDFLAGS ?=

ROOT     := $(CURDIR)
VERSION  := $(shell sed -n 's/^local _M = { _VERSION = "\(.*\)" }/\1/p' lua/gqlgw/init.lua)
PREFIX   ?= /usr/local
OPENRESTY_PREFIX ?= /usr/local/openresty
LIBDIR    = $(PREFIX)/lib/gqlgw
DEPS     := $(ROOT)/.deps
OPENRESTY:= $(DEPS)/openresty
NGINX    := $(OPENRESTY)/nginx/sbin/nginx
RESTY    := $(OPENRESTY)/bin/resty
B        := build

LIB_SRC  = src/util.c src/errors.c src/lexer.c src/parser.c src/json.c src/schema.c src/values.c \
           src/sdl.c src/gqlb.c src/routes.c src/validate.c src/plan.c src/introspect.c src/api.c
LIB_OBJ  = $(patsubst src/%.c,$(B)/obj/%.o,$(LIB_SRC))
# gqlc only (not in libgql.so): TOML config, nginx/Lua rendering
GQLC_SRC = src/toml.c src/config.c src/render.c
GQLC_OBJ = $(patsubst src/%.c,$(B)/obj/%.o,$(GQLC_SRC))
HDRS     = $(wildcard src/*.h)

SAN      = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

.PHONY: all lib install uninstall image docker-test clean test test-c test-t t-config test-config test-oracle test-toml fuzz deps schemas memcached-start memcached-stop dns-start dns-stop bench

all: $(B)/libgql.so $(B)/gqlc

lib: $(B)/libgql.so

src/prelude.inc: src/prelude.graphqls scripts/embed_prelude.sh
	./scripts/embed_prelude.sh $< $@

$(B)/obj/sdl.o: src/prelude.inc

# default libgql.so path written by gqlc render (same PREFIX for make and make install)
$(B)/obj/config.o: ALLCFLAGS += -DGQLGW_DEFAULT_LIB='"$(LIBDIR)/libgql.so"'

$(B)/obj/%.o: src/%.c $(HDRS)
	@mkdir -p $(dir $@)
	$(CC) $(ALLCFLAGS) -c -o $@ $<

$(B)/libgql.so: $(LIB_OBJ)
	$(CC) -shared $(LDFLAGS) -o $@ $(LIB_OBJ) -lm

$(B)/libgql.a: $(LIB_OBJ)
	ar rcs $@ $(LIB_OBJ)

$(B)/gqlc: src/gqlc.c $(GQLC_OBJ) $(B)/libgql.a
	$(CC) $(ALLCFLAGS) -fvisibility=default -o $@ src/gqlc.c $(GQLC_OBJ) $(B)/libgql.a -lm

# ---- install / Docker image ------------------------------------------------

install: all
	install -d $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(LIBDIR) $(DESTDIR)$(OPENRESTY_PREFIX)/site/lualib/gqlgw
	install -m 755 $(B)/gqlc $(DESTDIR)$(PREFIX)/bin/gqlc
	install -m 644 $(B)/libgql.so $(DESTDIR)$(LIBDIR)/libgql.so
	install -m 644 lua/gqlgw/*.lua $(DESTDIR)$(OPENRESTY_PREFIX)/site/lualib/gqlgw/

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/gqlc $(DESTDIR)$(LIBDIR)/libgql.so
	rm -rf $(DESTDIR)$(OPENRESTY_PREFIX)/site/lualib/gqlgw
	-rmdir $(DESTDIR)$(LIBDIR)

image:
	docker build -t gqlgw:$(VERSION) -t gqlgw:latest .

docker-test:
	tests/docker/run.sh

# ---- tests -----------------------------------------------------------------

# C unit tests, built with sanitizers
$(B)/test_c: tests/c/*.c tests/c/*.h $(LIB_SRC) $(GQLC_SRC) $(HDRS) src/prelude.inc
	@mkdir -p $(B)
	$(CC) -std=gnu99 $(WARN) $(SAN) -Isrc -o $@ tests/c/*.c $(LIB_SRC) $(GQLC_SRC) -lm

test-c: $(B)/test_c
	$(B)/test_c

# toml-test corpus (TOML 1.1.0), decoder built with sanitizers
$(B)/toml_decode: tests/toml/decode.c src/toml.c src/util.c $(HDRS)
	@mkdir -p $(B)
	$(CC) -std=gnu99 $(WARN) $(SAN) -Isrc -o $@ tests/toml/decode.c src/toml.c src/util.c -lm

test-toml: $(B)/toml_decode
	python3 -I tests/toml/run.py

# graphql-js differential tests (validation, introspection)
test-oracle: $(B)/gqlc
	node tests/oracle/run.mjs

# compile the SDL fixtures used by Test::Nginx
T_SCHEMAS = $(patsubst %.graphqls,%.gqlb,$(wildcard t/schemas/*.graphqls))
t/schemas/%.gqlb: t/schemas/%.graphqls $(B)/gqlc
	$(B)/gqlc compile -q -o $@ $<

schemas: $(T_SCHEMAS)

# configurations rendered by gqlc for t/006-config.t and t/007-server.t
T_ENV = TEST_NGINX_SERVER_PORT=1984 GQLGW_TEST_ROOT=$(ROOT) GQLGW_TEST_TLS_PORT=19843 \
        GQLGW_TEST_ADMIN_PORT=19844 GQLGW_TEST_PORT=19845 GQLGW_TEST_RESOLVER=127.0.0.1:21253

t-config: all schemas
	./scripts/test-certs.sh t/config/run/tls
	$(T_ENV) $(B)/gqlc render -q -c t/config/gqlgw.toml -o t/config/run --locations-only
	$(T_ENV) $(B)/gqlc render -q -c t/config/server.toml -o t/config/run-server

test-t: all schemas t-config memcached-start dns-start
	PATH=$(OPENRESTY)/nginx/sbin:$(OPENRESTY)/bin:$$PATH \
	PERL5LIB=$(DEPS)/perl/lib/perl5 \
	TEST_NGINX_ROOT=$(ROOT) TEST_NGINX_MEMCACHED_PORT=21211 TEST_NGINX_SERVER_PORT=1984 \
	TEST_NGINX_GQLGW_TLS_PORT=19843 TEST_NGINX_GQLGW_ADMIN_PORT=19844 TEST_NGINX_GQLGW_PORT=19845 \
	prove -r t/

# gqlgw.toml: error messages, golden renderings, nginx -t on them
test-config: all schemas
	tests/config/run.sh

test: test-c test-toml test-config test-oracle test-t

# ---- fuzzing (clang + libFuzzer) ---------------------------------------------

FUZZ_TIME ?= 60
$(B)/fuzz_%: tests/fuzz/fuzz_%.c $(LIB_SRC) $(GQLC_SRC) $(HDRS) src/prelude.inc
	@mkdir -p $(B)
	clang -std=gnu99 -g -O1 -fsanitize=fuzzer,address,undefined -Isrc -o $@ $< $(LIB_SRC) $(GQLC_SRC) -lm

$(B)/harness.gqlb: tests/oracle/harness.graphqls $(B)/gqlc
	$(B)/gqlc compile -q -o $@ -r $(B)/harness.routes.lua $<

fuzz: $(B)/fuzz_parse $(B)/fuzz_query $(B)/fuzz_toml $(B)/fuzz_config $(B)/harness.gqlb
	@mkdir -p $(B)/corpus_parse $(B)/corpus_query $(B)/corpus_toml $(B)/corpus_config $(B)/fuzz_artifacts
	$(B)/fuzz_parse tests/fuzz/regressions/*
	$(B)/fuzz_parse -max_total_time=$(FUZZ_TIME) -dict=tests/fuzz/graphql.dict -artifact_prefix=$(B)/fuzz_artifacts/parse- \
		$(B)/corpus_parse tests/corpus tests/fuzz/seeds
	GQL_FUZZ_SCHEMA=$(B)/harness.gqlb $(B)/fuzz_query -max_total_time=$(FUZZ_TIME) -max_len=4096 \
		-dict=tests/fuzz/graphql.dict -artifact_prefix=$(B)/fuzz_artifacts/query- $(B)/corpus_query tests/fuzz/seeds
	$(B)/fuzz_toml -max_total_time=$(FUZZ_TIME) -max_len=4096 -dict=tests/fuzz/toml.dict \
		-artifact_prefix=$(B)/fuzz_artifacts/toml- $(B)/corpus_toml tests/fuzz/toml_seeds
	$(B)/fuzz_config tests/fuzz/regressions_config/*
	$(B)/fuzz_config -max_total_time=$(FUZZ_TIME) -max_len=4096 -dict=tests/fuzz/toml.dict \
		-artifact_prefix=$(B)/fuzz_artifacts/config- $(B)/corpus_config tests/fuzz/config_seeds

# ---- deps / services ---------------------------------------------------------

deps:
	./scripts/deps.sh

memcached-start:
	@./scripts/memcached.sh start 21211

memcached-stop:
	@./scripts/memcached.sh stop 21211

# answers "localhost" for the nginx resolver of t/006-config.t (no /etc/hosts there)
dns-start:
	@python3 -I scripts/dns.py start 21253

dns-stop:
	@python3 -I scripts/dns.py stop 21253

bench: all
	./bench/run.sh

clean:
	rm -rf $(B) t/servroot* t/schemas/*.gqlb t/schemas/*.routes.lua t/config/run t/config/run-server
