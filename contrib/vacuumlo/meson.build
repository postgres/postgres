# Copyright (c) 2022-2024, PostgreSQL Global Development Group

vacuumlo_sources = files(
  'vacuumlo.c',
)

if host_system == 'windows'
  vacuumlo_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'vacuumlo',
    '--FILEDESC', 'vacuumlo - removes orphaned large objects',])
endif

vacuumlo = executable('vacuumlo',
  vacuumlo_sources,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args,
)
contrib_targets += vacuumlo

tests += {
  'name': 'vacuumlo',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
    ],
  },
}
