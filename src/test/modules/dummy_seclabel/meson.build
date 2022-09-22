# FIXME: prevent install during main install, but not during test :/
dummy_seclabel = shared_module('dummy_seclabel',
  ['dummy_seclabel.c'],
  kwargs: pg_mod_args,
)
testprep_targets += dummy_seclabel

install_data(
  'dummy_seclabel.control',
  'dummy_seclabel--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'dummy_seclabel',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'dummy_seclabel',
    ],
  },
}
