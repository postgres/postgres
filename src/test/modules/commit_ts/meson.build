# Copyright (c) 2022-2025, PostgreSQL Global Development Group

tests += {
  'name': 'commit_ts',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'commit_timestamp',
    ],
    # Disabled because these tests require "track_commit_timestamp = on",
    # which typical runningcheck users do not have (e.g. buildfarm clients).
    'runningcheck': false,
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
