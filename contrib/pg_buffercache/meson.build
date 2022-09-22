pg_buffercache = shared_module('pg_buffercache',
  files(
    'pg_buffercache_pages.c',
  ),
  kwargs: contrib_mod_args,
)
contrib_targets += pg_buffercache

install_data(
  'pg_buffercache--1.0--1.1.sql',
  'pg_buffercache--1.1--1.2.sql',
  'pg_buffercache--1.2--1.3.sql',
  'pg_buffercache--1.2.sql',
  'pg_buffercache.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_buffercache',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_buffercache',
    ],
  },
}
