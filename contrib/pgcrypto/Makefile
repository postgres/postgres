#
# $Header: /cvsroot/pgsql/contrib/pgcrypto/Makefile,v 1.7 2001/09/23 04:12:44 momjian Exp $
#

subdir = contrib/pgcrypto
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# either 'builtin', 'mhash', 'openssl'
cryptolib = builtin

# either 'builtin', 'system'
cryptsrc = builtin

# Random source, preferred order:
# 'dev'      - read from random device
#
# 'openssl'  - use openssl PRNG.
#              Note that currently pgcrypto does not do any
#              entropy feeding to it
#              This works ofcouse only with cryptolib = openssl
#
# 'silly'    - use libc random() - very weak
random = dev
random_dev = \"/dev/urandom\"

##########################

ifeq ($(cryptolib), builtin)
CRYPTO_CFLAGS =
CRYPTO_LDFLAGS =
SRCS = md5.c sha1.c internal.c blf.c rijndael.c
endif

ifeq ($(cryptolib), openssl)
CRYPTO_CFLAGS = -I/usr/include/openssl
CRYPTO_LDFLAGS = -lcrypto
SRCS = openssl.c
endif

ifeq ($(cryptolib), mhash)
CRYPTO_CFLAGS = -I/usr/local/include
CRYPTO_LDFLAGS = -L/usr/local/lib -lmcrypt -lmhash -lltdl
SRCS = mhash.c
endif

ifeq ($(cryptsrc), builtin)
SRCS += crypt-blowfish.c crypt-des.c crypt-md5.c 
else
CRYPTO_CFLAGS += -DPX_SYSTEM_CRYPT
endif

ifeq ($(random), dev)
CRYPTO_CFLAGS += -DRAND_DEV=$(random_dev)
endif
ifeq ($(random), openssl)
CRYPTO_CFLAGS += -DRAND_OPENSSL
endif
ifeq ($(random), silly)
CRYPTO_CFLAGS += -DRAND_SILLY
endif

NAME	:= pgcrypto
SRCS	+= pgcrypto.c px.c px-hmac.c px-crypt.c misc.c \
		crypt-gensalt.c random.c
OBJS	:= $(SRCS:.c=.o)
SHLIB_LINK := $(CRYPTO_LDFLAGS)
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 1

override CPPFLAGS	+= $(CRYPTO_CFLAGS) -I$(srcdir) 
override DLLLIBS := $(BE_DLLLIBS) $(DLLLIBS)
rpath :=

all: all-lib $(NAME).sql

include $(top_srcdir)/src/Makefile.shlib

$(NAME).sql: $(NAME).sql.in
	sed 's,@MODULE_FILENAME@,$$libdir/$(NAME),g' $< >$@

rijndael.o: rijndael.tbl

rijndael.tbl:
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPRINT_TABS rijndael.c -o gen-rtab
	./gen-rtab > rijndael.tbl

install: all installdirs
	$(INSTALL_SHLIB) $(shlib)	$(DESTDIR)$(pkglibdir)/pgcrypto$(DLSUFFIX)
	$(INSTALL_DATA) $(NAME).sql	$(DESTDIR)$(datadir)/contrib/$(NAME).sql
	$(INSTALL_DATA) README.$(NAME)	$(DESTDIR)$(docdir)/contrib/README.$(NAME)

installdirs:
	$(mkinstalldirs) $(pkglibdir) $(datadir)/contrib $(docdir)/contrib

uninstall: uninstall-lib
	rm -f $(DESTDIR)$(pkglibdir)/pgcrypto$(DLSUFFIX) $(datadir)/contrib/$(NAME).sql $(docdir)/contrib/README.$(NAME)

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) $(NAME).sql gen-rtab
