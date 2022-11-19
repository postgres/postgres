tests += {
  'name': 'commit_ts',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'commit_timestamp',
    ],
  },
  'tap': {
    'tests': [
      't/001_base.pl',
      't/002_standby.pl',
      't/003_standby_2.pl',
      't/004_restart.pl',
    ],
  },
}
