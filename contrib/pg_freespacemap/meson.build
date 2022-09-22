pg_freespacemap = shared_module('pg_freespacemap',
  files(
    'pg_freespacemap.c',
  ),
  kwargs: contrib_mod_args,
)
contrib_targets += pg_freespacemap

install_data(
  'pg_freespacemap--1.0--1.1.sql',
  'pg_freespacemap--1.1--1.2.sql',
  'pg_freespacemap--1.1.sql',
  'pg_freespacemap.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_freespacemap',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_freespacemap',
    ],
    'regress_args': [
      '--temp-config', files('pg_freespacemap.conf')
    ],
  },
}
