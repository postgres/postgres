# src/test/modules/Makefile

subdir = src/test/modules
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

SUBDIRS = \
		  brin \
		  commit_ts \
		  delay_execution \
		  dummy_index_am \
		  dummy_seclabel \
		  libpq_pipeline \
		  plsample \
		  spgist_name_ops \
		  test_bloomfilter \
		  test_copy_callbacks \
		  test_custom_rmgrs \
		  test_ddl_deparse \
		  test_dsa \
		  test_dsm_registry \
		  test_escape \
		  test_extensions \
		  test_ginpostinglist \
		  test_integerset \
		  test_json_parser \
		  test_lfind \
		  test_misc \
		  test_oat_hooks \
		  test_parser \
		  test_pg_dump \
		  test_predtest \
		  test_radixtree \
		  test_rbtree \
		  test_regex \
		  test_resowner \
		  test_rls_hooks \
		  test_shm_mq \
		  test_slru \
		  test_tidstore \
		  unsafe_tests \
		  worker_spi \
		  xid_wraparound


ifeq ($(enable_injection_points),yes)
SUBDIRS += injection_points gin typcache
else
ALWAYS_SUBDIRS += injection_points gin typcache
endif

ifeq ($(with_ssl),openssl)
SUBDIRS += ssl_passphrase_callback
else
ALWAYS_SUBDIRS += ssl_passphrase_callback
endif

# Test runs an LDAP server, so only run if ldap is in PG_TEST_EXTRA
ifeq ($(with_ldap),yes)
ifneq (,$(filter ldap,$(PG_TEST_EXTRA)))
SUBDIRS += ldap_password_func
else
ALWAYS_SUBDIRS += ldap_password_func
endif
else
ALWAYS_SUBDIRS += ldap_password_func
endif

$(recurse)
$(recurse_always)
