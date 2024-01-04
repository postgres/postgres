# Copyright (c) 2022-2024, PostgreSQL Global Development Group

tests += {
  'name': 'ssl',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {
      'with_ssl': ssl_library,
      'OPENSSL': openssl.found() ? openssl.path() : '',
    },
    'tests': [
      't/001_ssltests.pl',
      't/002_scram.pl',
      't/003_sslinfo.pl',
    ],
  },
}
