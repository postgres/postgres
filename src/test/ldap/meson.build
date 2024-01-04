# Copyright (c) 2022-2024, PostgreSQL Global Development Group

tests += {
  'name': 'ldap',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_auth.pl',
      't/002_bindpasswd.pl',
    ],
    'env': {
      'with_ldap': ldap.found() ? 'yes' : 'no',
    },
  },
}
