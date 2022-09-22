tcn = shared_module('tcn',
  files(
    'tcn.c',
  ),
  kwargs: contrib_mod_args,
)
contrib_targets += tcn

install_data(
  'tcn--1.0.sql',
  'tcn.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'tcn',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'isolation': {
    'specs': [
      'tcn',
    ],
    'regress_args': ['--load-extension=tcn'],
  },
}
