PREFIX ?= /usr
SBINDIR ?= $(PREFIX)/bin
CC ?= gcc
CFLAGS += -Wall -pthread
LDFLAGS += -g
LIBS = -lpthread

SOURCES = main.c reader.c write.c write2.c showev.c suinput.c

ifneq ($(inotify),no)
	GCC_FLAGS += -DWITH_INOTIFY
endif

all: build netevent devname

build:
	-mkdir build

build/%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $*.c -MMD -MF build/$*.d -MT $@

netevent: $(patsubst %.cpp,build/%.o,$(SOURCES))
	$(CC) $(LDFLAGS) -o $@ $^ $(LIBS)

devname: build/devname.o
	$(CC) -o $@ $^

install: all
	install -m 755 -p -t "$(DESTDIR)$(SBINDIR)" netevent devname

clean:
	-rm -rf build
	-rm -f netevent devname

-include build/*.d
