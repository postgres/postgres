pg_checksums = executable('pg_checksums',
  ['pg_checksums.c'],
  include_directories: [timezone_inc],
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
bin_targets += pg_checksums

tests += {
  'name': 'pg_checksums',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
      't/002_actions.pl',
    ],
  },
}

subdir('po', if_found: libintl)
