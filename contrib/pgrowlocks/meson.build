# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pgrowlocks_sources = files(
  'pgrowlocks.c',
)

if host_system == 'windows'
  pgrowlocks_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pgrowlocks',
    '--FILEDESC', 'pgrowlocks - display row locking information',])
endif

pgrowlocks = shared_module('pgrowlocks',
  pgrowlocks_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pgrowlocks

install_data(
  'pgrowlocks--1.0--1.1.sql',
  'pgrowlocks--1.1--1.2.sql',
  'pgrowlocks--1.2.sql',
  'pgrowlocks.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'pgrowlocks',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'isolation': {
    'specs': [
      'pgrowlocks',
    ],
    'regress_args': ['--load-extension=pgrowlocks'],
  },
}
