# Copyright (c) 2022-2024, PostgreSQL Global Development Group

libpq_uri_regress_sources = files(
  'libpq_uri_regress.c',
)

if host_system == 'windows'
  libpq_uri_regress_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'libpq_uri_regress',
    '--FILEDESC', 'libpq test program',])
endif

testprep_targets += executable('libpq_uri_regress',
  libpq_uri_regress_sources,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false,
  }
)


libpq_testclient_sources = files(
  'libpq_testclient.c',
)

if host_system == 'windows'
  libpq_testclient_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'libpq_testclient',
    '--FILEDESC', 'libpq test program',])
endif

testprep_targets += executable('libpq_testclient',
  libpq_testclient_sources,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false,
  }
)
