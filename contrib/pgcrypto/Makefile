#
# $Header: /cvsroot/pgsql/contrib/pgcrypto/Makefile,v 1.1 2000/10/31 13:11:28 petere Exp $
#

subdir = contrib/pgcrypto
top_builddir = ../..
include $(top_builddir)/src/Makefile.global

# either 'builtin', 'mhash', 'openssl', 'krb5'
cryptolib = builtin

##########################

ifeq ($(cryptolib), builtin)
SRCS= md5.c sha1.c internal.c
endif

ifeq ($(cryptolib), openssl)
cryptoinc := -I/usr/include/openssl
cryptolib := -lcrypto
SRCS = openssl.c
endif

ifeq ($(cryptolib), mhash)
cryptoinc=
cryptolib=-lmhash
SRCS=mhash.c
endif

ifeq ($(cryptolib), krb5)
cryptoinc=-I/usr/include
cryptolib=-ldes
SRCS=krb.c
endif

NAME	:= pgcrypto
SRCS	+= pgcrypto.c
OBJS	:= $(SRCS:.c=.o)
SO_MAJOR_VERSION = 0
SO_MINOR_VERSION = 1

override CPPFLAGS	+= -I$(srcdir) $(cryptoinc)

all: all-lib $(NAME).sql

include $(top_srcdir)/src/Makefile.shlib

$(NAME).sql: $(NAME).sql.in
	sed 's,@MODULE_FILENAME@,$(libdir)/contrib/pgcrypto$(DLSUFFIX),g' $< >$@

install: all installdirs
	$(INSTALL_SHLIB) $(shlib)	$(DESTDIR)$(libdir)/contrib/pgcrypto$(DLSUFFIX)
	$(INSTALL_DATA) $(NAME).sql	$(DESTDIR)$(datadir)/contrib/$(NAME).sql
	$(INSTALL_DATA) README.$(NAME)	$(DESTDIR)$(docdir)/contrib/README.$(NAME)

installdirs:
	$(mkinstalldirs) $(libdir)/contrib $(datadir)/contrib $(docdir)/contrib

uninstall: uninstall-lib
	rm -f $(DESTDIR)$(libdir)/contrib/pgcrypto$(DLSUFFIX) $(datadir)/contrib/$(NAME).sql $(docdir)/contrib/README.$(NAME)

clean distclean maintainer-clean: clean-lib
	rm -f $(OBJS) $(NAME).sql
