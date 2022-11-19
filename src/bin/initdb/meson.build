initdb_sources = files(
  'findtimezone.c',
  'initdb.c'
)

initdb_sources += timezone_localtime_source

#fixme: reimplement libpq_pgport logic

if host_system == 'windows'
  initdb_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'initdb',
    '--FILEDESC', 'initdb - initialize a new database cluster',])
endif

initdb = executable('initdb',
  initdb_sources,
  include_directories: [timezone_inc],
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
