unaccent = shared_module('unaccent',
  files(
    'unaccent.c',
  ),
  kwargs: contrib_mod_args,
)
contrib_targets += unaccent

install_data(
  'unaccent--1.0--1.1.sql',
  'unaccent--1.1.sql',
  'unaccent.control',
  kwargs: contrib_data_args,
)

install_data(
  'unaccent.rules',
  install_dir: dir_data / 'tsearch_data'
)

# XXX: Implement downlo
tests += {
  'name': 'unaccent',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'unaccent',
    ],
    'regress_args': ['--encoding=UTF8'],
  },
}
