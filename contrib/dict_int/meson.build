# Copyright (c) 2022-2024, PostgreSQL Global Development Group

dict_int_sources = files(
  'dict_int.c',
)

if host_system == 'windows'
  dict_int_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'dict_int',
    '--FILEDESC', 'dict_int - add-on dictionary template for full-text search',])
endif

dict_int = shared_module('dict_int',
  dict_int_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += dict_int

install_data(
  'dict_int.control',
  'dict_int--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'dict_int',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'dict_int',
    ],
  },
}
