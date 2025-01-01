# Copyright (c) 2022-2025, PostgreSQL Global Development Group

file_fdw_sources = files(
  'file_fdw.c',
)

if host_system == 'windows'
  file_fdw_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'file_fdw',
    '--FILEDESC', 'file_fdw - foreign data wrapper for files',])
endif

file_fdw = shared_module('file_fdw',
  file_fdw_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += file_fdw

install_data(
  'file_fdw.control',
  'file_fdw--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'file_fdw',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'file_fdw',
    ],
  },
}
