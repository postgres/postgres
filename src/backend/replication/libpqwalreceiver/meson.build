# Copyright (c) 2022-2025, PostgreSQL Global Development Group

libpqwalreceiver_sources = files(
  'libpqwalreceiver.c',
)

if host_system == 'windows'
  libpqwalreceiver_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pqwalreceiver',
    '--FILEDESC', 'libpqwalreceiver - receive WAL during streaming replication',])
endif

libpqwalreceiver = shared_module('pqwalreceiver',
  libpqwalreceiver_sources,
  kwargs: pg_mod_args + {
    'name_prefix': 'lib',
    'dependencies': pg_mod_args['dependencies'] + [libpq],
  }
)

backend_targets += libpqwalreceiver
