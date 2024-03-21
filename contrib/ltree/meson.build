# Copyright (c) 2022-2024, PostgreSQL Global Development Group

ltree_sources = files(
  '_ltree_gist.c',
  '_ltree_op.c',
  'crc32.c',
  'lquery_op.c',
  'ltree_gist.c',
  'ltree_io.c',
  'ltree_op.c',
  'ltxtquery_io.c',
  'ltxtquery_op.c',
)

# .. so that includes of ltree/ltree.h work
ltree_inc = include_directories('.', '../')

if host_system == 'windows'
  ltree_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'ltree',
    '--FILEDESC', 'ltree - hierarchical label data type',])
endif

ltree = shared_module('ltree',
  ltree_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += ltree

install_data(
  'ltree.control',
  'ltree--1.0--1.1.sql',
  'ltree--1.1.sql',
  'ltree--1.1--1.2.sql',
  'ltree--1.2--1.3.sql',
  kwargs: contrib_data_args,
)

install_headers(
  'ltree.h',
  install_dir: dir_include_extension / 'ltree',
)

tests += {
  'name': 'ltree',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'ltree',
    ],
  },
}
