pg_waldump_sources = files(
  'compat.c',
  'pg_waldump.c',
  'rmgrdesc.c',
)

pg_waldump_sources += rmgr_desc_sources
pg_waldump_sources += xlogreader_sources
pg_waldump_sources += files('../../backend/access/transam/xlogstats.c')

pg_waldump = executable('pg_waldump',
  pg_waldump_sources,
  dependencies: [frontend_code, lz4, zstd],
  c_args: ['-DFRONTEND'], # needed for xlogreader et al
  kwargs: default_bin_args,
)
bin_targets += pg_waldump

tests += {
  'name': 'pg_waldump',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
    ],
  },
}

subdir('po', if_found: libintl)
