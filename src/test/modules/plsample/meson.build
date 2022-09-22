# FIXME: prevent install during main install, but not during test :/
plsample = shared_module('plsample',
  ['plsample.c'],
  kwargs: pg_mod_args,
)
testprep_targets += plsample

install_data(
  'plsample.control',
  'plsample--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'plsample',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'plsample',
    ],
  },
}
