# libsquish — context-mixing compressor
#
# Targets:
#   make            build libsquish.so, libsquish.a, and the squish CLI
#   make test       build + run the test suite
#   make dll        cross-compile squish.dll + squish.exe (needs mingw-w64)
#   make windows-dll   build squish.dll + squish.exe with MSVC (needs cl.exe
#                       on PATH, e.g. a VS "Developer Command Prompt")
#   make install    install to $(PREFIX) (default /usr/local)
#   make clean

CC       ?= gcc
OPT      ?= -O3 -funroll-loops
WARN      = -Wall -Wextra
THREADS  ?= -pthread
CFLAGS   ?= $(OPT)
PREFIX   ?= /usr/local
DESTDIR  ?=

VERSION   = 1.0.0
SOMAJOR   = 1
SONAME    = libsquish.so.$(SOMAJOR)

MINGW    ?= x86_64-w64-mingw32-gcc
MINGW_AR ?= x86_64-w64-mingw32-ar

CL       ?= cl.exe
CLFLAGS  ?= /nologo /O2 /W3

all: libsquish.so squish

# ---- shared library ---------------------------------------------------------
libsquish.so.$(VERSION): squish.c squish.h
	$(CC) $(CFLAGS) $(THREADS) $(WARN) -fPIC -fvisibility=hidden -DSQUISH_BUILD \
	    -shared -Wl,-soname,$(SONAME) -o $@ squish.c -lm

libsquish.so: libsquish.so.$(VERSION)
	ln -sf libsquish.so.$(VERSION) $(SONAME)
	ln -sf $(SONAME) libsquish.so

# ---- static library ---------------------------------------------------------
squish.o: squish.c squish.h
	$(CC) $(CFLAGS) $(THREADS) $(WARN) -c -o $@ squish.c

libsquish.a: squish.o
	ar rcs $@ $^

# ---- CLI (statically linked against the lib) ---------------------------------
squish: squish_cli.c libsquish.a
	$(CC) $(CFLAGS) $(THREADS) $(WARN) -o $@ squish_cli.c libsquish.a -lm

# ---- Windows DLL + CLI (cross-compile; or use cl.exe /DSQUISH_BUILD_DLL) ------
dll: squish.dll squish.exe

squish.dll: squish.c squish.h
	$(MINGW) $(CFLAGS) $(WARN) -DSQUISH_BUILD_DLL -shared \
	    -o $@ squish.c -Wl,--out-implib,libsquish.dll.a

squish-win.o: squish.c squish.h
	$(MINGW) $(CFLAGS) $(WARN) -c -o $@ squish.c

libsquish-win.a: squish-win.o
	$(MINGW_AR) rcs $@ $^

squish.exe: squish_cli.c libsquish-win.a
	$(MINGW) $(CFLAGS) $(WARN) -o $@ squish_cli.c libsquish-win.a -lm

# ---- Windows DLL + CLI via MSVC (native build; run under a Developer Command
#      Prompt or after vcvarsall.bat so cl.exe is on PATH) ----------------------
# Not a squish.dll/squish.exe file rule: it would collide with the mingw
# rules above. Producing the same output filenames from either toolchain is
# intentional — pick whichever is available on the host.
windows-dll:
	$(CL) $(CLFLAGS) /LD /DSQUISH_BUILD_DLL /Fe:squish.dll squish.c
	$(CL) $(CLFLAGS) /DSQUISH_DLL /Fe:squish.exe squish_cli.c squish.lib

# ---- tests / examples ---------------------------------------------------------
test: tests/test_squish
	./tests/test_squish

tests/test_squish: tests/test_squish.c libsquish.a
	$(CC) $(CFLAGS) $(THREADS) $(WARN) -I. -o $@ tests/test_squish.c libsquish.a -lm

example: examples/example
examples/example: examples/example.c libsquish.so
	$(CC) $(CFLAGS) $(THREADS) $(WARN) -I. -o $@ examples/example.c -L. -lsquish -lm \
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
	printf 'prefix=%s\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: squish\nDescription: context-mixing compressor\nVersion: %s\nLibs: -L$${libdir} -lsquish -lm -pthread\nCflags: -I$${includedir}\n' \
	    "$(PREFIX)" "$(VERSION)" > $(DESTDIR)$(PREFIX)/lib/pkgconfig/squish.pc

clean:
	rm -f libsquish.so libsquish.so.* libsquish.a squish.o squish \
	      squish.dll libsquish.dll.a squish-win.o libsquish-win.a squish.exe \
	      squish.obj squish_cli.obj squish.lib squish.exp \
	      tests/test_squish examples/example

.PHONY: all dll windows-dll test example install clean
