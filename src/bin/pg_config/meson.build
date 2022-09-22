pg_config = executable('pg_config',
  ['pg_config.c'],
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
bin_targets += pg_config

tests += {
  'name': 'pg_config',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_pg_config.pl',
    ],
  },
}

subdir('po', if_found: libintl)
