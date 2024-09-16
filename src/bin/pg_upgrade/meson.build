# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pg_upgrade_sources = files(
  'check.c',
  'controldata.c',
  'dump.c',
  'exec.c',
  'file.c',
  'function.c',
  'info.c',
  'option.c',
  'parallel.c',
  'pg_upgrade.c',
  'relfilenumber.c',
  'server.c',
  'tablespace.c',
  'task.c',
  'util.c',
  'version.c',
)

if host_system == 'windows'
  pg_upgrade_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_upgrade',
    '--FILEDESC', 'pg_upgrade - an in-place binary upgrade utility'])
endif

pg_upgrade = executable('pg_upgrade',
  pg_upgrade_sources,
  c_pch: pch_postgres_fe_h,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args,
)
bin_targets += pg_upgrade


tests += {
  'name': 'pg_upgrade',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {'with_icu': icu.found() ? 'yes' : 'no'},
    'tests': [
      't/001_basic.pl',
      't/002_pg_upgrade.pl',
      't/003_logical_slots.pl',
      't/004_subscription.pl',
    ],
    'test_kwargs': {'priority': 40}, # pg_upgrade tests are slow
  },
}

subdir('po', if_found: libintl)
