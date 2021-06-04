# Copyright (c) 2024, PostgreSQL Global Development Group

oauthtest_sources = files(
  'oauthtest.c',
)

if host_system == 'windows'
  oauthtest_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'oauthtest',
    '--FILEDESC', 'passthrough module to validate OAuth tests',
  ])
endif

oauthtest = shared_module('oauthtest',
  oauthtest_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += oauthtest
