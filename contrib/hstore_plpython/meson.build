# Copyright (c) 2022-2025, PostgreSQL Global Development Group

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

if llvm.found()
  pyinc = python3_inst.get_variable('INCLUDEPY')
  llvm_contrib_hstore_plpython_cflags = [
    '-I@SOURCE_ROOT@/src/pl/plpython',
    '-I@SOURCE_ROOT@/contrib',
    '-I@SOURCE_ROOT@/contrib/hstore',
    '-DPLPYTHON_LIBNAME="plpython3"',
    '-I@0@'.format(pyinc),
  ]

  llvm_gen_contrib_hstore_plpython = generator(llvm_irgen_command,
    arguments: llvm_irgen_args + bitcode_cflags + pg_mod_c_args + llvm_contrib_hstore_plpython_cflags,
    depends: hstore_plpython,
    depfile: '@BASENAME@.c.bc.d',
    output: '@PLAINNAME@.bc',
  )

  bc_gen_custom_contrib += llvm_gen_contrib_hstore_plpython.process(hstore_plpython_sources)
endif


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
