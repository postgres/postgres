# Copyright (c) 2022-2025, PostgreSQL Global Development Group

if not perl_dep.found()
  subdir_done()
endif

hstore_plperl_sources = files(
  'hstore_plperl.c',
)

if host_system == 'windows'
  hstore_plperl_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'hstore_plperl',
    '--FILEDESC', 'hstore_plperl - hstore transform for plperl',])
endif

hstore_plperl = shared_module('hstore_plperl',
  hstore_plperl_sources,
  include_directories: [plperl_inc, hstore_inc],
  kwargs: contrib_mod_args + {
    'dependencies': [perl_dep, contrib_mod_args['dependencies']],
    'install_rpath': ':'.join(mod_install_rpaths + ['@0@/CORE'.format(archlibexp)]),
    'build_rpath': '@0@/CORE'.format(archlibexp),
  },
)

if llvm.found()
  llvm_contrib_hstore_plperl_cflags = [
    '-I@BUILD_ROOT@/src/pl/plperl',
    '-I@SOURCE_ROOT@/src/pl/plperl',
    '-I@SOURCE_ROOT@/contrib',
    '-I@SOURCE_ROOT@/contrib/hstore',
  ]

  llvm_gen_contrib_hstore_plperl = generator(llvm_irgen_command,
    arguments: llvm_irgen_args + bitcode_cflags + pg_mod_c_args + llvm_contrib_hstore_plperl_cflags + perl_ccflags,
    depends: hstore_plperl,
    depfile: '@BASENAME@.c.bc.d',
    output: '@PLAINNAME@.bc',
  )

  bc_gen_custom_contrib += llvm_gen_contrib_hstore_plperl.process(hstore_plperl_sources)
endif

install_data(
  'hstore_plperl.control',
  'hstore_plperl--1.0.sql',
  kwargs: contrib_data_args,
)

install_data(
  'hstore_plperlu.control',
  'hstore_plperlu--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'hstore_plperl',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'hstore_plperl',
      'hstore_plperlu',
      'create_transform',
    ],
  },
}
