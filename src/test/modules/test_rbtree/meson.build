# Copyright (c) 2022-2023, PostgreSQL Global Development Group

# FIXME: prevent install during main install, but not during test :/

test_rbtree_sources = files(
  'test_rbtree.c',
)

if host_system == 'windows'
  test_rbtree_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_rbtree',
    '--FILEDESC', 'test_rbtree - test code for red-black tree library',])
endif

test_rbtree = shared_module('test_rbtree',
  test_rbtree_sources,
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
