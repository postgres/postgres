libpqwalreceiver_sources = files(
  'libpqwalreceiver.c',
)

libpqwalreceiver = shared_module('pqwalreceiver',
  libpqwalreceiver_sources,
  kwargs: pg_mod_args + {
    'name_prefix': 'lib',
    'dependencies': pg_mod_args['dependencies'] + [libpq],
  }
)

backend_targets += libpqwalreceiver
