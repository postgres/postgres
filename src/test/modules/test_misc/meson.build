# Copyright (c) 2022-2026, PostgreSQL Global Development Group

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
      't/007_catcache_inval.pl',
      't/008_replslot_single_user.pl',
      't/009_log_temp_files.pl',
      't/010_index_concurrently_upsert.pl',
    ],
    # The injection points are cluster-wide, so disable installcheck
    'runningcheck': false,
  },
}
