adminpack = shared_module('adminpack',
  ['adminpack.c'],
  kwargs: contrib_mod_args,
)
contrib_targets += adminpack

install_data(
  'adminpack.control',
  'adminpack--1.0.sql',
  'adminpack--1.0--1.1.sql',
  'adminpack--1.1--2.0.sql',
  'adminpack--2.0--2.1.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'adminpack',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': ['adminpack'],
  },
}
