# FIXME: prevent install during main install, but not during test :/
test_lfind = shared_module('test_lfind',
  ['test_lfind.c'],
  kwargs: pg_mod_args,
)
testprep_targets += test_lfind

install_data(
  'test_lfind.control',
  'test_lfind--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'test_lfind',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_lfind',
    ],
  },
}
