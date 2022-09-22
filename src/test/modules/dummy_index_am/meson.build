# FIXME: prevent install during main install, but not during test :/
dummy_index_am = shared_module('dummy_index_am',
  ['dummy_index_am.c'],
  kwargs: pg_mod_args,
)
testprep_targets += dummy_index_am

install_data(
  'dummy_index_am.control',
  'dummy_index_am--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'dummy_index_am',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'reloptions',
    ],
  },
}
