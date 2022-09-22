dict_int = shared_module('dict_int',
  files('dict_int.c'),
  kwargs: contrib_mod_args,
)
contrib_targets += dict_int

install_data(
  'dict_int.control',
  'dict_int--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'dict_int',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'dict_int',
    ],
  },
}
