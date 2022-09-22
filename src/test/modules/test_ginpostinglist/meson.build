# FIXME: prevent install during main install, but not during test :/
test_ginpostinglist = shared_module('test_ginpostinglist',
  ['test_ginpostinglist.c'],
  kwargs: pg_mod_args,
)
testprep_targets += test_ginpostinglist

install_data(
  'test_ginpostinglist.control',
  'test_ginpostinglist--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'test_ginpostinglist',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_ginpostinglist',
    ],
  },
}
