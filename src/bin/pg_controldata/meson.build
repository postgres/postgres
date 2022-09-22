pg_controldata = executable('pg_controldata',
  ['pg_controldata.c'],
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
bin_targets += pg_controldata

tests += {
  'name': 'pg_controldata',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_pg_controldata.pl',
    ],
  },
}

subdir('po', if_found: libintl)
