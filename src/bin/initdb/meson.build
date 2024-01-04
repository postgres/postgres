# Copyright (c) 2022-2024, PostgreSQL Global Development Group

initdb_sources = files(
  'findtimezone.c',
  'initdb.c'
)

initdb_sources += timezone_localtime_source

if host_system == 'windows'
  initdb_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'initdb',
    '--FILEDESC', 'initdb - initialize a new database cluster',])
endif

initdb = executable('initdb',
  initdb_sources,
  include_directories: [timezone_inc],
  # Note: it's important that we link to encnames.o from libpgcommon, not
  # from libpq, else we have risks of version skew if we run with a libpq
  # shared library from a different PG version.  Define
  # USE_PRIVATE_ENCODING_FUNCS to ensure that that happens.
  c_args: ['-DUSE_PRIVATE_ENCODING_FUNCS'],
  dependencies: [frontend_code, libpq, icu, icu_i18n],
  kwargs: default_bin_args,
)
bin_targets += initdb

tests += {
  'name': 'initdb',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {'with_icu': icu.found() ? 'yes' : 'no'},
    'tests': [
      't/001_initdb.pl',
    ],
  },
}

subdir('po', if_found: libintl)
