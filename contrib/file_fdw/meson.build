file_fdw = shared_module('file_fdw',
  files('file_fdw.c'),
  kwargs: contrib_mod_args,
)
contrib_targets += file_fdw

install_data(
  'file_fdw.control',
  'file_fdw--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'file_fdw',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'file_fdw',
    ],
  },
}
