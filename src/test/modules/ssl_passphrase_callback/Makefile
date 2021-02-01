# ssl_passphrase_callback Makefile

export with_ssl

MODULE_big = ssl_passphrase_func
OBJS = ssl_passphrase_func.o $(WIN32RES)
PGFILEDESC = "callback function to provide a passphrase"

TAP_TESTS = 1

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/ssl_passphrase_callback
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

SHLIB_LINK += $(filter -lssl -lcrypto -lssleay32 -leay32, $(LIBS))

# Targets to generate or remove the ssl certificate and key
# Normally not needed. Don't run these targets in a vpath build, the results
# won't be in the right place if you do.

# needs to agree with what's in the test script
PASS = FooBaR1

.PHONY: ssl-files ssl-files-clean

ssl-files:
	openssl req -new -x509 -days 10000 -nodes -out server.crt \
		-keyout server.ckey -subj "/CN=localhost"
	openssl rsa -aes256 -in server.ckey -out server.key -passout pass:$(PASS)
	rm server.ckey

ssl-files-clean:
	rm -f server.crt server.key
