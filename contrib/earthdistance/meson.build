# Copyright (c) 2022-2024, PostgreSQL Global Development Group

earthdistance_sources = files(
  'earthdistance.c',
)

if host_system == 'windows'
  earthdistance_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'earthdistance',
    '--FILEDESC', 'earthdistance - calculate distances on the surface of the Earth',])
endif

earthdistance = shared_module('earthdistance',
  earthdistance_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += earthdistance

install_data(
  'earthdistance.control',
  'earthdistance--1.0--1.1.sql',
  'earthdistance--1.1.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'earthdistance',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'earthdistance',
    ],
  },
}
