dblink_sources = files(
  'dblink.c',
)

dblink = shared_module('dblink',
  dblink_sources,
  kwargs: contrib_mod_args + {
    'dependencies': contrib_mod_args['dependencies'] + [libpq],
  },
)
contrib_targets += dblink

install_data(
  'dblink.control',
  'dblink--1.0--1.1.sql',
  'dblink--1.1--1.2.sql',
  'dblink--1.2.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'dblink',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'dblink',
    ],
    'regress_args': ['--dlpath', meson.build_root() / 'src/test/regress'],
  },
}
