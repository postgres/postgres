# Copyright (c) 2022-2025, PostgreSQL Global Development Group

mssql_compat_sources = files(
  'mssql_compat.c',
)

if host_system == 'windows'
  mssql_compat_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'mssql_compat',
    '--FILEDESC', 'mssql_compat - SQL Server compatible date functions',])
endif

mssql_compat = shared_module('mssql_compat',
  mssql_compat_sources,
  kwargs: contrib_mod_args,
)

contrib_targets += mssql_compat

install_data(
  'mssql_compat--1.0.sql',
  'mssql_compat.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'mssql_compat',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'mssql_compat',
    ],
  },
}

