# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_dump_common_sources = files(
  'compress_gzip.c',
  'compress_io.c',
  'compress_lz4.c',
  'compress_none.c',
  'compress_zstd.c',
  'connectdb.c',
  'dumputils.c',
  'filter.c',
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
  c_pch: pch_postgres_fe_h,
  dependencies: [frontend_code, libpq, lz4, zlib, zstd],
  kwargs: internal_lib_args,
)


pg_dump_sources = files(
  'common.c',
  'pg_dump.c',
  'pg_dump_sort.c',
)

if host_system == 'windows'
  pg_dump_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_dump',
    '--FILEDESC', 'pg_dump - backup one PostgreSQL database',])
endif

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

if host_system == 'windows'
  pg_dumpall_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_dumpall',
    '--FILEDESC', 'pg_dumpall - backup PostgreSQL databases'])
endif

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

if host_system == 'windows'
  pg_restore_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_restore',
    '--FILEDESC', 'pg_restore - restore PostgreSQL databases'])
endif

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
    'env': {
      'GZIP_PROGRAM': gzip.found() ? gzip.path() : '',
      'LZ4': program_lz4.found() ? program_lz4.path() : '',
      'ZSTD': program_zstd.found() ? program_zstd.path() : '',
      'with_icu': icu.found() ? 'yes' : 'no',
    },
    'tests': [
      't/001_basic.pl',
      't/002_pg_dump.pl',
      't/003_pg_dump_with_server.pl',
      't/004_pg_dump_parallel.pl',
      't/005_pg_dump_filterfile.pl',
      't/006_pg_dumpall.pl',
      't/010_dump_connstr.pl',
    ],
  },
}

subdir('po', if_found: libintl)
