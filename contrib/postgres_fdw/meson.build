postgres_fdw_sources = files(
  'connection.c',
  'deparse.c',
  'option.c',
  'postgres_fdw.c',
  'shippable.c',
)

postgres_fdw = shared_module('postgres_fdw',
  postgres_fdw_sources,
  kwargs: contrib_mod_args + {
    'dependencies': contrib_mod_args['dependencies'] + [libpq],
  },
)
contrib_targets += postgres_fdw

install_data(
  'postgres_fdw.control',
  'postgres_fdw--1.0.sql',
  'postgres_fdw--1.0--1.1.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'postgres_fdw',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'postgres_fdw',
    ],
    'regress_args': ['--dlpath', meson.build_root() / 'src/test/regress'],
  },
}
