# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pg_combinebackup_sources = files(
  'pg_combinebackup.c',
  'backup_label.c',
  'copy_file.c',
  'load_manifest.c',
  'reconstruct.c',
  'write_manifest.c',
)

if host_system == 'windows'
  pg_combinebackup_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_combinebackup',
    '--FILEDESC', 'pg_combinebackup - combine incremental backups',])
endif

pg_combinebackup = executable('pg_combinebackup',
  pg_combinebackup_sources,
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
bin_targets += pg_combinebackup

tests += {
  'name': 'pg_combinebackup',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
      't/002_compare_backups.pl',
      't/003_timeline.pl',
      't/004_manifest.pl',
      't/005_integrity.pl',
      't/006_db_file_copy.pl',
      't/007_wal_level_minimal.pl',
      't/008_promote.pl',
      't/009_no_full_file.pl',
    ],
  }
}

subdir('po', if_found: libintl)
