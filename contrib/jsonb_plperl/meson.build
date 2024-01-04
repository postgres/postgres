# Copyright (c) 2022-2024, PostgreSQL Global Development Group

if not perl_dep.found()
  subdir_done()
endif

jsonb_plperl_sources = files(
  'jsonb_plperl.c',
)

if host_system == 'windows'
  jsonb_plperl_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'jsonb_plperl',
    '--FILEDESC', 'jsonb_plperl - jsonb transform for plperl',])
endif

jsonb_plperl = shared_module('jsonb_plperl',
  jsonb_plperl_sources,
  include_directories: [plperl_inc],
  kwargs: contrib_mod_args + {
    'dependencies': [perl_dep, contrib_mod_args['dependencies']],
    'install_rpath': ':'.join(mod_install_rpaths + ['@0@/CORE'.format(archlibexp)]),
    'build_rpath': '@0@/CORE'.format(archlibexp),
  },
)
contrib_targets += jsonb_plperl

install_data(
  'jsonb_plperl.control',
  'jsonb_plperl--1.0.sql',
  kwargs: contrib_data_args,
)

install_data(
  'jsonb_plperlu.control',
  'jsonb_plperlu--1.0.sql',
  kwargs: contrib_data_args,
)


tests += {
  'name': 'jsonb_plperl',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'jsonb_plperl',
      'jsonb_plperlu',
    ],
  },
}
