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

pg_basebackup = executable('pg_basebackup',
  'pg_basebackup.c',
  link_with: [pg_basebackup_common],
  dependencies: pg_basebackup_deps,
  kwargs: default_bin_args,
)
bin_targets += pg_basebackup

pg_receivewal = executable('pg_receivewal',
  'pg_receivewal.c',
  link_with: [pg_basebackup_common],
  dependencies: pg_basebackup_deps,
  kwargs: default_bin_args,
)
bin_targets += pg_receivewal

pg_recvlogical = executable('pg_recvlogical',
  'pg_recvlogical.c',
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
