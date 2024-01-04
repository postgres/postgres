# Copyright (c) 2022-2024, PostgreSQL Global Development Group

tests += {
  'name': 'unsafe_tests',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'rolenames',
      'alter_system_table',
      'guc_privs',
    ],
    'runningcheck': false,
  },
}
