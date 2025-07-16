# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pg_rewind_sources = files(
  'datapagemap.c',
  'file_ops.c',
  'filemap.c',
  'libpq_source.c',
  'local_source.c',
  'parsexlog.c',
  'pg_rewind.c',
  'timeline.c',
)

pg_rewind_sources += xlogreader_sources

if host_system == 'windows'
  pg_rewind_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_rewind',
    '--FILEDESC', 'pg_rewind - synchronize a data directory with another one forked from'])
endif

link_w  = []
include_dirs = [postgres_inc]

if percona_ext == true
  link_w = [pg_tde_frontend]
  include_dirs = [postgres_inc, pg_tde_inc]
endif

pg_rewind = executable('pg_rewind',
  pg_rewind_sources,
  dependencies: [frontend_code, libpq, lz4, zstd],
  c_args: ['-DFRONTEND'], # needed for xlogreader et al
  kwargs: default_bin_args,
  include_directories: include_dirs,
  link_with: link_w,
)
bin_targets += pg_rewind


tests += {
  'name': 'pg_rewind',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
      't/002_databases.pl',
      't/003_extrafiles.pl',
      't/004_pg_xlog_symlink.pl',
      't/005_same_timeline.pl',
      't/006_options.pl',
      't/007_standby_source.pl',
      't/008_min_recovery_point.pl',
      't/009_growing_files.pl',
      't/010_keep_recycled_wals.pl',
    ],
  },
}

subdir('po', if_found: libintl)
