# Copyright (c) 2022-2025, PostgreSQL Global Development Group

test_ddl_deparse_sources = files(
  'test_ddl_deparse.c',
)

if host_system == 'windows'
  test_ddl_deparse_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_ddl_deparse',
    '--FILEDESC', 'test_ddl_deparse - regression testing for DDL deparsing',])
endif

test_ddl_deparse = shared_module('test_ddl_deparse',
  test_ddl_deparse_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_ddl_deparse

test_install_data += files(
  'test_ddl_deparse.control',
  'test_ddl_deparse--1.0.sql',
)

tests += {
  'name': 'test_ddl_deparse',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_ddl_deparse',
      'create_extension',
      'create_schema',
      'create_type',
      'create_conversion',
      'create_domain',
      'create_sequence_1',
      'create_table',
      'create_transform',
      'alter_table',
      'create_view',
      'create_trigger',
      'create_rule',
      'comment_on',
      'alter_function',
      'alter_sequence',
      'alter_ts_config',
      'alter_type_enum',
      'opfamily',
      'defprivs',
      'matviews',
    ],
  },
}
