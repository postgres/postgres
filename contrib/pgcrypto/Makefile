#
# $PostgreSQL: pgsql/contrib/pgcrypto/Makefile,v 1.17 2005/07/06 16:14:42 tgl Exp $
#

# if you don't have OpenSSL, you can use libc random() or /dev/urandom
INT_CFLAGS = -DRAND_SILLY
#INT_CFLAGS = -DRAND_DEV=\"/dev/urandom\"

INT_SRCS = md5.c sha1.c internal.c blf.c rijndael.c

OSSL_CFLAGS = -DRAND_OPENSSL
OSSL_SRCS = openssl.c
OSSL_TESTS = des 3des cast5

CF_SRCS = $(if $(subst no,,$(with_openssl)), $(OSSL_SRCS), $(INT_SRCS))
CF_TESTS = $(if $(subst no,,$(with_openssl)), $(OSSL_TESTS))
CF_CFLAGS = $(if $(subst no,,$(with_openssl)), $(OSSL_CFLAGS), $(INT_CFLAGS))

PG_CPPFLAGS	= $(CF_CFLAGS)

SRCS		= pgcrypto.c px.c px-hmac.c px-crypt.c misc.c random.c \
		crypt-gensalt.c crypt-blowfish.c crypt-des.c \
		crypt-md5.c $(CF_SRCS)

MODULE_big	= pgcrypto
OBJS		= $(SRCS:.c=.o)
DOCS		= README.pgcrypto
DATA_built	= pgcrypto.sql
EXTRA_CLEAN	= gen-rtab

REGRESS = init md5 sha1 hmac-md5 hmac-sha1 blowfish rijndael \
	$(CF_TESTS) \
	crypt-des crypt-md5 crypt-blowfish crypt-xdes


ifdef USE_PGXS
PGXS = $(shell pg_config --pgxs)
include $(PGXS)
else
subdir = contrib/pgcrypto
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

# Add libraries that pgcrypto depends (or might depend) on into the
# shared library link.  (The order in which you list them here doesn't
# matter.)
SHLIB_LINK += $(filter -lcrypt -ldes -lcrypto -lssl, $(LIBS))
ifeq ($(PORTNAME), win32)
SHLIB_LINK += $(filter -leay32 -lssleay32, $(LIBS))
endif

# to make ws2_32.lib the last library (must occur after definition of PORTNAME)
ifeq ($(PORTNAME),win32)
SHLIB_LINK += -lwsock32 -lws2_32
endif


rijndael.o: rijndael.tbl

rijndael.tbl:
	$(CC) $(CPPFLAGS) $(CFLAGS) -DPRINT_TABS rijndael.c -o gen-rtab
	./gen-rtab > rijndael.tbl
