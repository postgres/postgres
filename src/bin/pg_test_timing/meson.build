pg_test_timing = executable('pg_test_timing',
  ['pg_test_timing.c'],
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
bin_targets += pg_test_timing

tests += {
  'name': 'pg_test_timing',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
    ],
  },
}

subdir('po', if_found: libintl)
