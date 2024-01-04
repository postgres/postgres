# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pg_waldump_sources = files(
  'compat.c',
  'pg_waldump.c',
  'rmgrdesc.c',
)

pg_waldump_sources += rmgr_desc_sources
pg_waldump_sources += xlogreader_sources
pg_waldump_sources += files('../../backend/access/transam/xlogstats.c')

if host_system == 'windows'
  pg_waldump_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_waldump',
    '--FILEDESC', 'pg_waldump - decode and display WA'])
endif

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
      't/002_save_fullpage.pl',
    ],
  },
}

subdir('po', if_found: libintl)
