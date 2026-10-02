# luna-browser — luna-ui + QuickJS web engine
#
#   make                 the browser: a window when GLFW is installed, plus the
#                        headless modes (CDP pipe, --screenshot, --dump-dom)
#   make LB_WINDOW=0     headless only
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

LB_WINDOW ?= $(shell pkg-config --exists glfw3 2>/dev/null && echo 1 || echo 0)
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

build/luna-browser.o: luna-browser.c build/luna-browser.js.h $(wildcard $(LUNA_UI)/*.h)
	$(CC) $(CFLAGS) $(WARN) -Ibuild -I$(QJS) -I$(LUNA_UI) -c $< -o $@

luna-browser: build/luna-browser.o $(QJS_OBJ)
	$(CC) $(CFLAGS) $^ -o $@ $(LIBS)

test: script-dom-test http-framing-test document-layout-test css-robust-test text-edit-test

script-dom-test: luna-browser
	python3 test/script_dom.py

document-layout-test: luna-browser
	python3 test/document_layout.py

text-edit-test: luna-browser
	python3 test/text_edit.py

css-robust-test: luna-browser
	python3 test/css_robust.py

http-framing-test: luna-browser
	python3 test/http_framing.py

clean:
	rm -rf build luna-browser

.PHONY: all test script-dom-test http-framing-test document-layout-test css-robust-test text-edit-test clean
