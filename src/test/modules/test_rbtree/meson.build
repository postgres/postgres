# FIXME: prevent install during main install, but not during test :/
test_rbtree = shared_module('test_rbtree',
  ['test_rbtree.c'],
  kwargs: pg_mod_args,
)
testprep_targets += test_rbtree

install_data(
  'test_rbtree.control',
  'test_rbtree--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'test_rbtree',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_rbtree',
    ],
  },
}
