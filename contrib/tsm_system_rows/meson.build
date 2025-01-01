# Copyright (c) 2022-2025, PostgreSQL Global Development Group

tsm_system_rows_sources = files(
  'tsm_system_rows.c',
)

if host_system == 'windows'
  tsm_system_rows_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'tsm_system_rows',
    '--FILEDESC', 'tsm_system_rows - TABLESAMPLE method which accepts number of rows as a limit',])
endif

tsm_system_rows = shared_module('tsm_system_rows',
  tsm_system_rows_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += tsm_system_rows

install_data(
  'tsm_system_rows--1.0.sql',
  'tsm_system_rows.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'tsm_system_rows',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'tsm_system_rows',
    ],
  },
}
