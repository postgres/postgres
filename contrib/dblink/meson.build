# Copyright (c) 2022-2024, PostgreSQL Global Development Group

dblink_sources = files(
  'dblink.c',
)

if host_system == 'windows'
  dblink_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'dblink',
    '--FILEDESC', 'dblink - connect to other PostgreSQL databases',])
endif

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
