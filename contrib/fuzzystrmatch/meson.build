fuzzystrmatch = shared_module('fuzzystrmatch',
  files(
    'fuzzystrmatch.c',
    'dmetaphone.c'
  ),
  kwargs: contrib_mod_args,
)
contrib_targets += fuzzystrmatch

install_data(
  'fuzzystrmatch.control',
  'fuzzystrmatch--1.0--1.1.sql',
  'fuzzystrmatch--1.1.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'fuzzystrmatch',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'fuzzystrmatch',
    ],
  },
}
