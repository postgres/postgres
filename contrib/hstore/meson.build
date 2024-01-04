# Copyright (c) 2022-2024, PostgreSQL Global Development Group

# .. so that includes of hstore/hstore.h work
hstore_inc = include_directories('.', '../')

hstore_sources = files(
  'hstore_compat.c',
  'hstore_gin.c',
  'hstore_gist.c',
  'hstore_io.c',
  'hstore_op.c',
  'hstore_subs.c',
)

if host_system == 'windows'
  hstore_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'hstore',
    '--FILEDESC', 'hstore - key/value pair data type',])
endif

hstore = shared_module('hstore',
  hstore_sources,
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args,
)
contrib_targets += hstore

install_data(
  'hstore.control',
  'hstore--1.1--1.2.sql',
  'hstore--1.2--1.3.sql',
  'hstore--1.3--1.4.sql',
  'hstore--1.4.sql',
  'hstore--1.4--1.5.sql',
  'hstore--1.5--1.6.sql',
  'hstore--1.6--1.7.sql',
  'hstore--1.7--1.8.sql',
  kwargs: contrib_data_args,
)

install_headers(
  'hstore.h',
  install_dir: dir_include_extension / 'hstore',
)

tests += {
  'name': 'hstore',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'hstore',
      'hstore_utf8',
    ],
  },
}
