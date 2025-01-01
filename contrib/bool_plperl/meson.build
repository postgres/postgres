# Copyright (c) 2022-2025, PostgreSQL Global Development Group

if not perl_dep.found()
  subdir_done()
endif

bool_plperl_sources = files(
  'bool_plperl.c',
)

if host_system == 'windows'
  bool_plperl_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'bool_plperl',
    '--FILEDESC', 'bool_plperl - bool transform for plperl',])
endif

bool_plperl = shared_module('bool_plperl',
  bool_plperl_sources,
  include_directories: [plperl_inc, include_directories('.')],
  kwargs: contrib_mod_args + {
    'dependencies': [perl_dep, contrib_mod_args['dependencies']],
    'install_rpath': ':'.join(mod_install_rpaths + ['@0@/CORE'.format(archlibexp)]),
    'build_rpath': '@0@/CORE'.format(archlibexp),
  },
)
contrib_targets += bool_plperl

install_data(
  'bool_plperl.control',
  'bool_plperl--1.0.sql',
  kwargs: contrib_data_args,
)

install_data(
  'bool_plperlu.control',
  'bool_plperlu--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'bool_plperl',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'bool_plperl',
      'bool_plperlu',
    ],
  },
}
