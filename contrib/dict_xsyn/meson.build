# Copyright (c) 2022-2025, PostgreSQL Global Development Group

dict_xsyn_sources = files(
  'dict_xsyn.c',
)

if host_system == 'windows'
  dict_xsyn_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'dict_xsyn',
    '--FILEDESC', 'dict_xsyn - add-on dictionary template for full-text search',])
endif

dict_xsyn = shared_module('dict_xsyn',
  dict_xsyn_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += dict_xsyn

install_data(
  'dict_xsyn.control',
  'dict_xsyn--1.0.sql',
  kwargs: contrib_data_args,
)

install_data(
  'xsyn_sample.rules',
  kwargs: contrib_data_args + {
    'install_dir': dir_data / 'tsearch_data'
  }
)

tests += {
  'name': 'dict_xsyn',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'dict_xsyn',
    ],
  },
}
