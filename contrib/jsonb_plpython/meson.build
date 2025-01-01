# Copyright (c) 2022-2025, PostgreSQL Global Development Group

if not python3_dep.found()
  subdir_done()
endif

jsonb_plpython_sources = files(
  'jsonb_plpython.c',
)

if host_system == 'windows'
  jsonb_plpython_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'jsonb_plpython3',
    '--FILEDESC', 'jsonb_plpython - jsonb transform for plpython',])
endif

jsonb_plpython = shared_module('jsonb_plpython3',
  jsonb_plpython_sources,
  include_directories: [plpython_inc],
  c_args: ['-DPLPYTHON_LIBNAME="plpython3"'],
  kwargs: contrib_mod_args + {
    'dependencies': [python3_dep, contrib_mod_args['dependencies']],
  },
)
contrib_targets += jsonb_plpython

install_data(
  'jsonb_plpython3u.control',
  'jsonb_plpython3u--1.0.sql',
  kwargs: contrib_data_args,
)

jsonb_plpython_regress = [
  'jsonb_plpython'
]

tests += {
  'name': 'jsonb_plpython',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': jsonb_plpython_regress,
  },
}
