pg_dump_common_sources = files(
  'compress_io.c',
  'dumputils.c',
  'parallel.c',
  'pg_backup_archiver.c',
  'pg_backup_custom.c',
  'pg_backup_db.c',
  'pg_backup_directory.c',
  'pg_backup_null.c',
  'pg_backup_tar.c',
  'pg_backup_utils.c',
)

pg_dump_common = static_library('libpgdump_common',
  pg_dump_common_sources,
  dependencies: [frontend_code, libpq, zlib],
  kwargs: internal_lib_args,
)


pg_dump_sources = files(
  'common.c',
  'pg_dump.c',
  'pg_dump_sort.c',
)

pg_dump = executable('pg_dump',
  pg_dump_sources,
  link_with: [pg_dump_common],
  dependencies: [frontend_code, libpq, zlib],
  kwargs: default_bin_args,
)
bin_targets += pg_dump


pg_dumpall_sources = files(
  'pg_dumpall.c',
)

pg_dumpall = executable('pg_dumpall',
  pg_dumpall_sources,
  link_with: [pg_dump_common],
  dependencies: [frontend_code, libpq, zlib],
  kwargs: default_bin_args,
)
bin_targets += pg_dumpall


pg_restore_sources = files(
  'pg_restore.c',
)

pg_restore = executable('pg_restore',
  pg_restore_sources,
  link_with: [pg_dump_common],
  dependencies: [frontend_code, libpq, zlib],
  kwargs: default_bin_args,
)
bin_targets += pg_restore

tests += {
  'name': 'pg_dump',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
      't/002_pg_dump.pl',
      't/003_pg_dump_with_server.pl',
      't/010_dump_connstr.pl',
    ],
  },
}

subdir('po', if_found: libintl)
