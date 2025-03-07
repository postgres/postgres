# contrib/pg_tde/Makefile

PGFILEDESC = "pg_tde access method"
MODULE_big = pg_tde
EXTENSION = pg_tde
DATA = pg_tde--1.0-rc.sql

REGRESS_OPTS = --temp-config $(top_srcdir)/contrib/pg_tde/pg_tde.conf
REGRESS = toast_decrypt \
access_control \
alter_index \
change_access_method \
insert_update_delete \
keyprovider_dependency \
pg_tde_is_encrypted \
relocate \
subtransaction \
tablespace \
vault_v2_test
TAP_TESTS = 1

OBJS = src/encryption/enc_tde.o \
src/encryption/enc_aes.o \
src/access/pg_tde_tdemap.o \
src/access/pg_tde_xlog.o \
src/access/pg_tde_xlog_encrypt.o \
src/transam/pg_tde_xact_handler.o \
src/keyring/keyring_curl.o \
src/keyring/keyring_file.o \
src/keyring/keyring_vault.o \
src/keyring/keyring_kmip.o \
src/keyring/keyring_kmip_ereport.o \
src/keyring/keyring_api.o \
src/catalog/tde_keyring.o \
src/catalog/tde_keyring_parse_opts.o \
src/catalog/tde_principal_key.o \
src/common/pg_tde_shmem.o \
src/common/pg_tde_utils.o \
src/smgr/pg_tde_smgr.o \
src/pg_tde_defs.o \
src/pg_tde_event_capture.o \
src/pg_tde_guc.o \
src/pg_tde.o \
src/libkmip/libkmip/src/kmip.o \
src/libkmip/libkmip/src/kmip_bio.o \
src/libkmip/libkmip/src/kmip_locate.o \
src/libkmip/libkmip/src/kmip_memset.o

SCRIPTS_built = src/pg_tde_alter_key_provider

EXTRA_CLEAN += src/pg_tde_alter_key_provider.o

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
override PG_CPPFLAGS += -I$(CURDIR)/src/include -I$(CURDIR)/src/libkmip/libkmip/include
include $(PGXS)
else
subdir = contrib/pg_tde
top_builddir = ../..
override PG_CPPFLAGS += -I$(top_srcdir)/$(subdir)/src/include  -I$(top_srcdir)/$(subdir)/src/libkmip/libkmip/include
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif

override SHLIB_LINK += -lcurl -lcrypto -lssl

src/pg_tde_alter_key_provider: src/pg_tde_alter_key_provider.o $(top_srcdir)/src/fe_utils/simple_list.o $(top_builddir)/src/libtde/libtde.a
	$(CC) -DFRONTEND $(CFLAGS) $^ $(LDFLAGS) $(LDFLAGS_EX) $(LIBS) -o $@$(X)

# Fetches typedefs list for PostgreSQL core and merges it with typedefs defined in this project.
# https://wiki.postgresql.org/wiki/Running_pgindent_on_non-core_code_or_development_code
update-typedefs:
	wget -q -O - "https://buildfarm.postgresql.org/cgi-bin/typedefs.pl?branch=REL_17_STABLE" | cat - typedefs.list | sort | uniq > typedefs-full.list

# Indents projects sources.
indent:
	pgindent --typedefs=typedefs-full.list --excludes=pgindent_excludes .

.PHONY: update-typedefs indent
