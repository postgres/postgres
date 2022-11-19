pg_stat_statements_sources = files(
  'pg_stat_statements.c',
)

if host_system == 'windows'
  pg_stat_statements_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_stat_statements',
    '--FILEDESC', 'pg_stat_statements - execution statistics of SQL statements',])
endif

pg_stat_statements = shared_module('pg_stat_statements',
  pg_stat_statements_sources,
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
