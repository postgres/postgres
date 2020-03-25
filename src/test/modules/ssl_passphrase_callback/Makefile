# ssl_passphrase Makefile

export with_openssl

MODULE_big = ssl_passphrase_func
OBJS = ssl_passphrase_func.o $(WIN32RES)
PGFILEDESC = "callback function to provide a passphrase"

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

check: prove-check

prove-check: ssl_passphrase_func$(DLSUFFIX) | temp-install
	@echo running prove ...
	$(prove_check)
