# contrib/pgcrypto/Makefile

ZLIB_TST = pgp-compression
ZLIB_OFF_TST = pgp-zlib-DISABLED

CF_PGP_TESTS = $(if $(subst no,,$(with_zlib)), $(ZLIB_TST), $(ZLIB_OFF_TST))

OBJS = \
	$(WIN32RES) \
	crypt-blowfish.o \
	crypt-des.o \
	crypt-gensalt.o \
	crypt-md5.o \
	mbuf.o \
	openssl.o \
	pgcrypto.o \
	pgp-armor.o \
	pgp-cfb.o \
	pgp-compress.o \
	pgp-decrypt.o \
	pgp-encrypt.o \
	pgp-info.o \
	pgp-mpi.o \
	pgp-mpi-openssl.o \
	pgp-pgsql.o \
	pgp-pubdec.o \
	pgp-pubenc.o \
	pgp-pubkey.o \
	pgp-s2k.o \
	pgp.o \
	px-crypt.o \
	px-hmac.o \
	px.o

MODULE_big	= pgcrypto

EXTENSION = pgcrypto
DATA = pgcrypto--1.3.sql pgcrypto--1.2--1.3.sql pgcrypto--1.1--1.2.sql \
	pgcrypto--1.0--1.1.sql
PGFILEDESC = "pgcrypto - cryptographic functions"

REGRESS = init md5 sha1 hmac-md5 hmac-sha1 blowfish rijndael \
	sha2 des 3des cast5 \
	crypt-des crypt-md5 crypt-blowfish crypt-xdes \
	pgp-armor pgp-decrypt pgp-encrypt pgp-encrypt-md5 $(CF_PGP_TESTS) \
	pgp-pubkey-decrypt pgp-pubkey-encrypt pgp-info

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
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
SHLIB_LINK += $(filter -lcrypto -lz, $(LIBS))
ifeq ($(PORTNAME), win32)
SHLIB_LINK += $(filter -leay32, $(LIBS))
# those must be at the end
SHLIB_LINK += -lws2_32
endif
