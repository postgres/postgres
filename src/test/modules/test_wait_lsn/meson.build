# Copyright (c) 2026, PostgreSQL Global Development Group

test_wait_lsn_sources = files(
  'test_wait_lsn.c',
)

if host_system == 'windows'
  test_wait_lsn_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_wait_lsn',
    '--FILEDESC', 'test_wait_lsn - test code for WAIT FOR LSN',])
endif

test_wait_lsn = shared_module('test_wait_lsn',
  test_wait_lsn_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_wait_lsn

test_install_data += files(
  'test_wait_lsn.control',
  'test_wait_lsn--1.0.sql',
)
