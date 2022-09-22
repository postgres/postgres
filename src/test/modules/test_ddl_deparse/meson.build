# FIXME: prevent install during main install, but not during test :/
test_ddl_deparse = shared_module('test_ddl_deparse',
  ['test_ddl_deparse.c'],
  kwargs: pg_mod_args,
)
testprep_targets += test_ddl_deparse

install_data(
  'test_ddl_deparse.control',
  'test_ddl_deparse--1.0.sql',
  kwargs: contrib_data_args,
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
