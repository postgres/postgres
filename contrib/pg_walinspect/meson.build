# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pg_walinspect_sources = files('pg_walinspect.c')

if host_system == 'windows'
  pg_walinspect_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_walinspect',
    '--FILEDESC', 'pg_walinspect - functions to inspect contents of PostgreSQL Write-Ahead Log',])
endif

pg_walinspect = shared_module('pg_walinspect',
  pg_walinspect_sources,
  kwargs: contrib_mod_args + {
      'dependencies': contrib_mod_args['dependencies'],
  },
)
contrib_targets += pg_walinspect

install_data(
  'pg_walinspect.control',
  'pg_walinspect--1.0.sql',
  'pg_walinspect--1.0--1.1.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_walinspect',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_walinspect',
      'oldextversions',
    ],
    # Disabled because these tests require "wal_level=replica", which
    # some runningcheck users do not have (e.g. buildfarm clients).
    'regress_args': ['--temp-config', files('walinspect.conf')],
    'runningcheck': false,
  },
}
