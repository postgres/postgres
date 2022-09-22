pg_stat_statements = shared_module('pg_stat_statements',
  files('pg_stat_statements.c'),
  kwargs: contrib_mod_args + {
    'dependencies': contrib_mod_args['dependencies'],
  },
)
contrib_targets += pg_stat_statements

install_data(
  'pg_stat_statements.control',
  'pg_stat_statements--1.4.sql',
  'pg_stat_statements--1.9--1.10.sql',
  'pg_stat_statements--1.8--1.9.sql',
  'pg_stat_statements--1.7--1.8.sql',
  'pg_stat_statements--1.6--1.7.sql',
  'pg_stat_statements--1.5--1.6.sql',
  'pg_stat_statements--1.4--1.5.sql',
  'pg_stat_statements--1.3--1.4.sql',
  'pg_stat_statements--1.2--1.3.sql',
  'pg_stat_statements--1.1--1.2.sql',
  'pg_stat_statements--1.0--1.1.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pg_stat_statements',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_stat_statements',
    ],
    'regress_args': ['--temp-config', files('pg_stat_statements.conf')],
  },
}
