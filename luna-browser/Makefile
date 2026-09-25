# luna-browser — luna-ui + QuickJS web engine
#
#   make                 headless build (CDP pipe, --screenshot, --dump-dom)
#   make LB_WINDOW=1     also an interactive window through luna-ui's host
#   make LB_NO_TLS=1     without OpenSSL (no https)
#   make test            headless self-tests

CC       ?= cc
LUNA_UI  ?= ../../luna-ui
QJS      := third_party/quickjs
OPT      ?= -O2
CFLAGS   ?= $(OPT) -g
CFLAGS   += -std=c11 -D_GNU_SOURCE
WARN     := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare \
            -Wno-missing-field-initializers
LIBS     := -lEGL -ldl -lm -lpthread

ifeq ($(LB_WINDOW),1)
CFLAGS   += -DLB_WINDOW
LIBS     += $(shell pkg-config --libs glfw3 2>/dev/null || echo -lglfw)
endif
ifeq ($(LB_NO_TLS),1)
CFLAGS   += -DLB_NO_TLS
else
LIBS     += $(shell pkg-config --libs openssl 2>/dev/null || echo -lssl -lcrypto)
endif

QJS_SRC  := $(QJS)/quickjs.c $(QJS)/libregexp.c $(QJS)/libunicode.c \
            $(QJS)/cutils.c $(QJS)/dtoa.c
QJS_OBJ  := $(patsubst $(QJS)/%.c,build/qjs/%.o,$(QJS_SRC))
QJS_DEFS := -DCONFIG_VERSION=\"$(shell cat $(QJS)/VERSION)\" -D_GNU_SOURCE -fwrapv

all: luna-browser

build/qjs/%.o: $(QJS)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(OPT) -g $(QJS_DEFS) -w -c $< -o $@

# The script prelude, as a C string literal.
build/luna-browser.js.h: luna-browser.js
	@mkdir -p build
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $< > $@

build/luna-browser.o: luna-browser.c build/luna-browser.js.h $(LUNA_UI)/luna-ui.h
	$(CC) $(CFLAGS) $(WARN) -Ibuild -I$(QJS) -I$(LUNA_UI) -c $< -o $@

luna-browser: build/luna-browser.o $(QJS_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

test: luna-browser
	./test/run.sh

clean:
	rm -rf build luna-browser

.PHONY: all test clean
