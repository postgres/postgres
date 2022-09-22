vacuumlo = executable('vacuumlo',
  ['vacuumlo.c'],
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
