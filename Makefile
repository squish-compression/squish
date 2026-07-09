# libsquish — context-mixing compressor
#
# Targets:
#   make            build libsquish.so, libsquish.a, and the squish CLI
#   make test       build + run the test suite
#   make dll        cross-compile squish.dll (needs x86_64-w64-mingw32-gcc)
#   make install    install to $(PREFIX) (default /usr/local)
#   make clean

CC       ?= gcc
OPT      ?= -O3 -funroll-loops
WARN      = -Wall -Wextra
CFLAGS   ?= $(OPT)
PREFIX   ?= /usr/local
DESTDIR  ?=

VERSION   = 1.0.0
SOMAJOR   = 1
SONAME    = libsquish.so.$(SOMAJOR)

MINGW    ?= x86_64-w64-mingw32-gcc

all: libsquish.so squish

# ---- shared library ---------------------------------------------------------
libsquish.so.$(VERSION): squish.c squish.h
	$(CC) $(CFLAGS) $(WARN) -fPIC -fvisibility=hidden -DSQUISH_BUILD \
	    -shared -Wl,-soname,$(SONAME) -o $@ squish.c -lm

libsquish.so: libsquish.so.$(VERSION)
	ln -sf libsquish.so.$(VERSION) $(SONAME)
	ln -sf $(SONAME) libsquish.so

# ---- static library ---------------------------------------------------------
squish.o: squish.c squish.h
	$(CC) $(CFLAGS) $(WARN) -c -o $@ squish.c

libsquish.a: squish.o
	ar rcs $@ $^

# ---- CLI (statically linked against the lib) ---------------------------------
squish: squish_cli.c libsquish.a
	$(CC) $(CFLAGS) $(WARN) -o $@ squish_cli.c libsquish.a -lm

# ---- Windows DLL (cross-compile; or use cl.exe /DSQUISH_BUILD_DLL) ------------
dll: squish.dll
squish.dll: squish.c squish.h
	$(MINGW) $(CFLAGS) $(WARN) -DSQUISH_BUILD_DLL -shared \
	    -o $@ squish.c -Wl,--out-implib,libsquish.dll.a

# ---- tests / examples ---------------------------------------------------------
test: tests/test_squish
	./tests/test_squish

tests/test_squish: tests/test_squish.c libsquish.a
	$(CC) $(CFLAGS) $(WARN) -I. -o $@ tests/test_squish.c libsquish.a -lm

example: examples/example
examples/example: examples/example.c libsquish.so
	$(CC) $(CFLAGS) $(WARN) -I. -o $@ examples/example.c -L. -lsquish -lm \
	    -Wl,-rpath,'$$ORIGIN/..'

# ---- install ------------------------------------------------------------------
install: libsquish.so libsquish.a squish
	install -d $(DESTDIR)$(PREFIX)/lib $(DESTDIR)$(PREFIX)/include \
	           $(DESTDIR)$(PREFIX)/bin $(DESTDIR)$(PREFIX)/lib/pkgconfig
	install -m 755 libsquish.so.$(VERSION) $(DESTDIR)$(PREFIX)/lib/
	ln -sf libsquish.so.$(VERSION) $(DESTDIR)$(PREFIX)/lib/$(SONAME)
	ln -sf $(SONAME) $(DESTDIR)$(PREFIX)/lib/libsquish.so
	install -m 644 libsquish.a $(DESTDIR)$(PREFIX)/lib/
	install -m 644 squish.h $(DESTDIR)$(PREFIX)/include/
	install -m 755 squish $(DESTDIR)$(PREFIX)/bin/
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: squish\nDescription: context-mixing compressor\nVersion: %s\nLibs: -L$${libdir} -lsquish -lm\nCflags: -I$${includedir}\n' \
	    "$(PREFIX)" "$(VERSION)" > $(DESTDIR)$(PREFIX)/lib/pkgconfig/squish.pc

clean:
	rm -f libsquish.so libsquish.so.* libsquish.a squish.o squish \
	      squish.dll libsquish.dll.a tests/test_squish examples/example

.PHONY: all dll test example install clean
