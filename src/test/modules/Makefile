# src/test/modules/Makefile

subdir = src/test/modules
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
		  brin \
		  commit_ts \
		  dummy_index_am \
		  dummy_seclabel \
		  snapshot_too_old \
		  test_bloomfilter \
		  test_ddl_deparse \
		  test_extensions \
		  test_ginpostinglist \
		  test_integerset \
		  test_misc \
		  test_parser \
		  test_pg_dump \
		  test_predtest \
		  test_rbtree \
		  test_rls_hooks \
		  test_shm_mq \
		  unsafe_tests \
		  worker_spi

ifeq ($(with_openssl),yes)
SUBDIRS += ssl_passphrase_callback
else
ALWAYS_SUBDIRS += ssl_passphrase_callback
endif

$(recurse)
$(recurse_always)
