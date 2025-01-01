# Copyright (c) 2022-2025, PostgreSQL Global Development Group

tablefunc_sources = files(
  'tablefunc.c',
)

if host_system == 'windows'
  tablefunc_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'tablefunc',
    '--FILEDESC', 'tablefunc - various functions that return tables',])
endif

tablefunc = shared_module('tablefunc',
  tablefunc_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += tablefunc

install_data(
  'tablefunc--1.0.sql',
  'tablefunc.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'tablefunc',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'tablefunc',
    ],
  },
}
