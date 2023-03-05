# Copyright (c) 2022-2023, PostgreSQL Global Development Group

tests += {
  'name': 'ssl',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {
      'with_ssl': get_option('ssl'),
      'OPENSSL': openssl.path(),
    },
    'tests': [
      't/001_ssltests.pl',
      't/002_scram.pl',
      't/003_sslinfo.pl',
    ],
  },
}
