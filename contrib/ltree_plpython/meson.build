# Copyright (c) 2022-2025, PostgreSQL Global Development Group

if not python3_dep.found()
  subdir_done()
endif

ltree_plpython_sources = files(
  'ltree_plpython.c',
)

if host_system == 'windows'
  ltree_plpython_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'ltree_plpython3',
    '--FILEDESC', 'ltree_plpython - ltree transform for plpython',])
endif

ltree_plpython = shared_module('ltree_plpython3',
  ltree_plpython_sources,
  include_directories: [plpython_inc, ltree_inc],
  c_args: ['-DPLPYTHON_LIBNAME="plpython3"'],
  kwargs: contrib_mod_args + {
    'dependencies': [python3_dep, contrib_mod_args['dependencies']],
  },
)
contrib_targets += ltree_plpython

install_data(
  'ltree_plpython3u--1.0.sql',
  'ltree_plpython3u.control',
  kwargs: contrib_data_args,
)

ltree_plpython_regress = [
  'ltree_plpython'
]

tests += {
  'name': 'ltree_plpython',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': ltree_plpython_regress,
    'regress_args': ['--load-extension=ltree'],
  },
}
