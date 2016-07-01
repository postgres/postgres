# src/test/modules/Makefile

subdir = src/test/modules
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
		  brin \
		  commit_ts \
		  dummy_seclabel \
		  snapshot_too_old \
		  test_ddl_deparse \
		  test_extensions \
		  test_parser \
		  test_pg_dump \
		  test_rls_hooks \
		  test_shm_mq \
		  worker_spi

all: submake-generated-headers

$(recurse)
