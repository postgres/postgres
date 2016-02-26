# contrib/contrib-global.mk

# file with extra config for temp build
ifdef TEMP_CONFIG
REGRESS_OPTS += --temp-config=$(TEMP_CONFIG)
endif

NO_PGXS = 1
include $(top_srcdir)/src/makefiles/pgxs.mk
