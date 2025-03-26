C_SRCS = slack.c \
	 slack-auth.c \
	 slack-cmd.c \
	 slack-message.c \
	 slack-conversation.c \
	 slack-channel.c \
	 slack-im.c \
	 slack-thread.c \
	 slack-user.c \
	 slack-rtm.c \
	 slack-blist.c \
	 slack-api.c \
	 slack-object.c \
	 slack-json.c \
	 slack-emoji.c \
	 purple-websocket.c \
	 json.c

RESOURCES_XMLS = slack-emoji.gresource.xml
RESOURCES_C_SRCS = $(RESOURCES_XMLS:.gresource.xml=-resource.c)
RESOURCES_C_HDRS = $(RESOURCES_XMLS:.gresource.xml=-resource.h)

C_SRCS += $(RESOURCES_C_SRCS)

# Object file names using 'Substitution Reference'
C_OBJS = $(C_SRCS:.c=.o)

%-resource.c: %.gresource.xml %-resource.h
	glib-compile-resources --internal --generate $< --target $@

%-resource.h: %.gresource.xml
	glib-compile-resources --internal --generate $< --target $@

PURPLE_MOD=purple

ifeq ($(OS),Windows_NT)

LIBNAME=libslack.dll
PIDGIN_TREE_TOP ?= ../pidgin-2.10.11
WIN32_DEV_TOP ?= $(PIDGIN_TREE_TOP)/../win32-dev
WIN32_CC ?= $(WIN32_DEV_TOP)/mingw-4.7.2/bin/gcc

PROGFILES32=${ProgramFiles(x86)}
ifndef PROGFILES32
PROGFILES32=$(PROGRAMFILES)
endif

CC = $(WIN32_DEV_TOP)/mingw-4.7.2/bin/gcc

DATA_ROOT_DIR_PURPLE:="$(PROGFILES32)/Pidgin"
PLUGIN_DIR_PURPLE:="$(DATA_ROOT_DIR_PURPLE)/plugins"
CFLAGS = \
    -g \
    -O2 \
    -Wall \
    -D_DEFAULT_SOURCE=1 \
    -D_XOPEN_SOURCE=1 \
    -std=c99 \
	-I$(PIDGIN_TREE_TOP)/libpurple \
	-I$(WIN32_DEV_TOP)/glib-2.28.8/include -I$(WIN32_DEV_TOP)/glib-2.28.8/include/glib-2.0 -I$(WIN32_DEV_TOP)/glib-2.28.8/lib/glib-2.0/include
LIBS = -L$(WIN32_DEV_TOP)/glib-2.28.8/lib -L$(PIDGIN_TREE_TOP)/libpurple -lpurple -lintl -lglib-2.0 -lgobject-2.0 -g -ggdb -static-libgcc -lz -lws2_32 

else

LIBNAME=libslack.so

PLUGIN_DIR_PURPLE:=$(DESTDIR)$(shell pkg-config --variable=plugindir $(PURPLE_MOD))
DATA_ROOT_DIR_PURPLE:=$(DESTDIR)$(shell pkg-config --variable=datarootdir $(PURPLE_MOD))
PKGS=$(PURPLE_MOD) glib-2.0 gobject-2.0 gio-2.0

CFLAGS = \
    -g \
    -O2 \
    -Wall \
    -Wno-error=strict-aliasing \
    -Wno-deprecated-declarations \
    -fPIC \
    -D_DEFAULT_SOURCE=1 \
    -D_XOPEN_SOURCE=1 \
    -std=c99 \
    $(shell pkg-config --cflags $(PKGS)) \
    $(LOCAL_CFLAGS)

LIBS = $(shell pkg-config --libs $(PKGS))

endif

.PHONY: all
all: $(LIBNAME)

LDFLAGS = -shared

json.%: json-parser/json.%
	cp $< $@

%.o: %.c
	$(CC) -c $(CFLAGS) -o $@ $<
%.E: %.c
	$(CC) -E $(CFLAGS) -o $@ $<

$(LIBNAME): $(C_OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

.PHONY: install install-user
install: $(LIBNAME)
	install -d $(PLUGIN_DIR_PURPLE)
	install $(LIBNAME) $(PLUGIN_DIR_PURPLE)
	for z in 16 22 48 ; do \
		install -d $(DATA_ROOT_DIR_PURPLE)/pixmaps/pidgin/protocols/$$z ; \
		install -m 0644 img/slack$$z.png $(DATA_ROOT_DIR_PURPLE)/pixmaps/pidgin/protocols/$$z/slack.png ; \
	done

install-user: $(LIBNAME)
	install -d $(HOME)/.purple/plugins
	install $(LIBNAME) $(HOME)/.purple/plugins

.PHONY: uninstall
uninstall: $(LIBNAME)
	rm $(PLUGIN_DIR_PURPLE)/$(LIBNAME)
	rm $(DATA_ROOT_DIR_PURPLE)/pixmaps/pidgin/protocols/16/slack.png
	rm $(DATA_ROOT_DIR_PURPLE)/pixmaps/pidgin/protocols/22/slack.png
	rm $(DATA_ROOT_DIR_PURPLE)/pixmaps/pidgin/protocols/48/slack.png

.PHONY: clean
clean:
	rm -f *.o $(LIBNAME) Makefile.dep

.PHONY: modversion
modversion:
	pkg-config --modversion $(PKGS)

Makefile.dep: $(C_SRCS)
	$(CC) -MM $(CFLAGS) $^ > Makefile.dep


test-slack-emoji: test-slack-emoji.c slack-emoji.o json.o slack-json.o $(RESOURCES_C_SRCS)
	$(CC) -o $@ \
	    $(shell pkg-config --cflags --libs $(PKGS)) \
	    -lm $^

.PHONY: test
test: test-slack-emoji
	for i in $< ; do ./$$i ; done

include Makefile.dep
