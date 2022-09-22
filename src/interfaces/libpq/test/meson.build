executable('libpq_uri_regress',
  files('libpq_uri_regress.c'),
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false,
  }
)

executable('libpq_testclient',
  files('libpq_testclient.c'),
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false,
  }
)
