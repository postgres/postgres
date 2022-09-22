tests += {
  'name': 'ldap',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_auth.pl',
    ],
    'env': {'with_ldap': ldap.found() ? 'yes' : 'no'},
  },
}
