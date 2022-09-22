tests += {
  'name': 'ssl',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'env': {'with_ssl': get_option('ssl')},
    'tests': [
      't/001_ssltests.pl',
      't/002_scram.pl',
      't/003_sslinfo.pl',
    ],
  },
}
