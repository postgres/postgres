# Copyright (c) 2022-2024, PostgreSQL Global Development Group

tests += {
  'name': 'test_misc',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {
       'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
    },
    'tests': [
      't/001_constraint_validation.pl',
      't/002_tablespace.pl',
      't/003_check_guc.pl',
      't/004_io_direct.pl',
      't/005_timeouts.pl',
      't/006_signal_autovacuum.pl',
    ],
  },
}
