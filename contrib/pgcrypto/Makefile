#
# $Header: /cvsroot/pgsql/contrib/pgcrypto/Makefile,v 1.5 2001/08/21 00:42:41 momjian Exp $
#

subdir = contrib/pgcrypto
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# either 'builtin', 'mhash', 'openssl'
cryptolib = builtin

# either 'builtin', 'system'
cryptsrc = builtin

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

NAME	:= pgcrypto
SRCS	+= pgcrypto.c px.c px-hmac.c px-crypt.c misc.c
OBJS	:= $(SRCS:.c=.o)
SHLIB_LINK := $(CRYPTO_LDFLAGS)
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 1

override CPPFLAGS	+= $(CRYPTO_CFLAGS) -I$(srcdir) 
override DLLLIBS := $(BE_DLLLIBS) $(DLLLIBS)

all: all-lib $(NAME).sql

include $(top_srcdir)/src/Makefile.shlib

$(NAME).sql: $(NAME).sql.in
	sed 's,@MODULE_FILENAME@,$(libdir)/contrib/pgcrypto$(DLSUFFIX),g' $< >$@

rijndael.o: rijndael.tbl

rijndael.tbl:
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPRINT_TABS rijndael.c -o gen-rtab
	./gen-rtab > rijndael.tbl

install: all installdirs
	$(INSTALL_SHLIB) $(shlib)	$(DESTDIR)$(libdir)/contrib/pgcrypto$(DLSUFFIX)
	$(INSTALL_DATA) $(NAME).sql	$(DESTDIR)$(datadir)/contrib/$(NAME).sql
	$(INSTALL_DATA) README.$(NAME)	$(DESTDIR)$(docdir)/contrib/README.$(NAME)

installdirs:
	$(mkinstalldirs) $(libdir)/contrib $(datadir)/contrib $(docdir)/contrib

uninstall: uninstall-lib
	rm -f $(DESTDIR)$(libdir)/contrib/pgcrypto$(DLSUFFIX) $(datadir)/contrib/$(NAME).sql $(docdir)/contrib/README.$(NAME)

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) $(NAME).sql gen-rtab
