# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pg_checksums_sources = files(
  'pg_checksums.c',
)

if host_system == 'windows'
  pg_checksums_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_checksums',
    '--FILEDESC', 'pg_checksums - verify data checksums in an offline cluster',])
endif

link_w  = []
include_dirs = [timezone_inc]

if percona_ext == true
  link_w = [pg_tde_frontend]
  include_dirs = [timezone_inc, pg_tde_inc]
endif

pg_checksums = executable('pg_checksums',
  pg_checksums_sources,
  include_directories: include_dirs,
  dependencies: [frontend_code],
  kwargs: default_bin_args,
  link_with: link_w,
)
bin_targets += pg_checksums

tests += {
  'name': 'pg_checksums',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
      't/002_actions.pl',
    ],
  },
}

subdir('po', if_found: libintl)
