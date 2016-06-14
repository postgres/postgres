# contrib/uuid-ossp/Makefile

MODULE_big = uuid-ossp
OBJS = uuid-ossp.o $(UUID_EXTRA_OBJS) $(WIN32RES)

EXTENSION = uuid-ossp
DATA = uuid-ossp--1.1.sql uuid-ossp--1.0--1.1.sql uuid-ossp--unpackaged--1.0.sql
PGFILEDESC = "uuid-ossp - UUID generation"

REGRESS = uuid_ossp

SHLIB_LINK += $(UUID_LIBS)

# We copy some needed files verbatim from pgcrypto
pgcrypto_src = $(top_srcdir)/contrib/pgcrypto

PG_CPPFLAGS = -I$(pgcrypto_src)

EXTRA_CLEAN = md5.c sha1.c

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/uuid-ossp
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

md5.c sha1.c: % : $(pgcrypto_src)/%
	rm -f $@ && $(LN_S) $< .
