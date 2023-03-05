# Copyright (c) 2022-2023, PostgreSQL Global Development Group

common_sources = files(
  'bbstreamer_file.c',
  'bbstreamer_gzip.c',
  'bbstreamer_inject.c',
  'bbstreamer_lz4.c',
  'bbstreamer_tar.c',
  'bbstreamer_zstd.c',
  'receivelog.c',
  'streamutil.c',
  'walmethods.c',
)

pg_basebackup_deps = [frontend_code, libpq, lz4, zlib, zstd]
pg_basebackup_common = static_library('libpg_basebackup_common',
  common_sources,
  dependencies: pg_basebackup_deps,
  kwargs: internal_lib_args,
)

pg_basebackup_sources = files(
  'pg_basebackup.c',
)

if host_system == 'windows'
  pg_basebackup_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_basebackup',
    '--FILEDESC', 'pg_basebackup - streaming WAL and backup receivers',])
endif

pg_basebackup = executable('pg_basebackup',
  pg_basebackup_sources,
  link_with: [pg_basebackup_common],
  dependencies: pg_basebackup_deps,
  kwargs: default_bin_args,
)
bin_targets += pg_basebackup


pg_receivewal_sources = files(
  'pg_receivewal.c',
)

if host_system == 'windows'
  pg_receivewal_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_receivewal',
    '--FILEDESC', 'pg_receivewal - streaming WAL and backup receivers',])
endif

pg_receivewal = executable('pg_receivewal',
  pg_receivewal_sources,
  link_with: [pg_basebackup_common],
  dependencies: pg_basebackup_deps,
  kwargs: default_bin_args,
)
bin_targets += pg_receivewal


pg_recvlogical_sources = files(
  'pg_recvlogical.c',
)

if host_system == 'windows'
  pg_recvlogical_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_recvlogical',
    '--FILEDESC', 'pg_recvlogical - streaming WAL and backup receivers',])
endif

pg_recvlogical = executable('pg_recvlogical',
  pg_recvlogical_sources,
  link_with: [pg_basebackup_common],
  dependencies: pg_basebackup_deps,
  kwargs: default_bin_args,
)
bin_targets += pg_recvlogical

tests += {
  'name': 'pg_basebackup',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {'GZIP_PROGRAM': gzip.path(),
            'TAR': tar.path(),
            'LZ4': program_lz4.found() ? program_lz4.path() : '',
            'ZSTD': program_zstd.found() ? program_zstd.path() : ''},
    'tests': [
      't/010_pg_basebackup.pl',
      't/020_pg_receivewal.pl',
      't/030_pg_recvlogical.pl',
    ],
  },
}

subdir('po', if_found: libintl)
