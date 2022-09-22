isn_sources = files(
  'isn.c',
)

isn = shared_module('isn',
  isn_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += isn

install_data(
  'isn.control',
  'isn--1.0--1.1.sql',
  'isn--1.1--1.2.sql',
  'isn--1.1.sql',
  kwargs: contrib_data_args,
)

install_headers(
  'isn.h',
  install_dir: dir_include_extension / 'isn',
)

tests += {
  'name': 'isn',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'isn',
    ],
  },
}
