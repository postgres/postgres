lo_sources = files(
  'lo.c',
)

lo = shared_module('lo',
  lo_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += lo

install_data(
  'lo.control',
  'lo--1.0--1.1.sql',
  'lo--1.1.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'lo',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'lo',
    ],
  },
}
