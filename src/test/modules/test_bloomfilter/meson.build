# FIXME: prevent install during main install, but not during test :/
test_bloomfilter = shared_module('test_bloomfilter',
  ['test_bloomfilter.c'],
  kwargs: pg_mod_args,
)
testprep_targets += test_bloomfilter

install_data(
  'test_bloomfilter.control',
  'test_bloomfilter--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'test_bloomfilter',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_bloomfilter',
    ],
  },
}
