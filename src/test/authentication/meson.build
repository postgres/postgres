# Copyright (c) 2022-2024, PostgreSQL Global Development Group

tests += {
  'name': 'authentication',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_password.pl',
      't/002_saslprep.pl',
      't/003_peer.pl',
      't/004_file_inclusion.pl',
      't/005_sspi.pl',
      't/006_login_trigger.pl',
    ],
  },
}
