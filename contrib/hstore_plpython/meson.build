# Copyright (c) 2022-2024, PostgreSQL Global Development Group

if not python3_dep.found()
  subdir_done()
endif

hstore_plpython_sources = files(
  'hstore_plpython.c',
)

if host_system == 'windows'
  hstore_plpython_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'hstore_plpython3',
    '--FILEDESC', 'hstore_plpython - hstore transform for plpython',])
endif

hstore_plpython = shared_module('hstore_plpython3',
  hstore_plpython_sources,
  include_directories: [plpython_inc, hstore_inc, ],
  c_args: ['-DPLPYTHON_LIBNAME="plpython3"'],
  kwargs: contrib_mod_args + {
    'dependencies': [python3_dep, contrib_mod_args['dependencies']],
  },
)
contrib_targets += hstore_plpython

install_data(
  'hstore_plpython3u--1.0.sql',
  'hstore_plpython3u.control',
  kwargs: contrib_data_args,
)

hstore_plpython_regress = [
  'hstore_plpython'
]

tests += {
  'name': 'hstore_plpython',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': hstore_plpython_regress,
    'regress_args': ['--load-extension=hstore'],
  },
}
