pg_walinspect_sources = files('pg_walinspect.c')

pg_walinspect = shared_module('pg_walinspect',
  pg_walinspect_sources,
  kwargs: contrib_mod_args + {
      'dependencies': contrib_mod_args['dependencies'],
  },
)
contrib_targets += pg_walinspect

install_data(
  'pg_walinspect.control',
  'pg_walinspect--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_walinspect',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_walinspect',
    ],
    'regress_args': ['--temp-config', files('walinspect.conf')],
  },
}
