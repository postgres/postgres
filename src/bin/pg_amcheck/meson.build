# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_amcheck_sources = files(
  'pg_amcheck.c',
)

if host_system == 'windows'
  pg_amcheck_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_amcheck',
    '--FILEDESC', 'pg_amcheck - detect corruption within database relations',])
endif

pg_amcheck = executable('pg_amcheck',
  pg_amcheck_sources,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args,
)
bin_targets += pg_amcheck

tests += {
  'name': 'pg_amcheck',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
      't/002_nonesuch.pl',
      't/003_check.pl',
      't/004_verify_heapam.pl',
      't/005_opclass_damage.pl',
    ],
  },
}

subdir('po', if_found: libintl)
