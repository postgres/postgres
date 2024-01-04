# Copyright (c) 2022-2024, PostgreSQL Global Development Group

tcn_sources = files(
  'tcn.c',
)

if host_system == 'windows'
  tcn_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'tcn',
    '--FILEDESC', 'tcn - trigger function notifying listeners',])
endif

tcn = shared_module('tcn',
  tcn_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += tcn

install_data(
  'tcn--1.0.sql',
  'tcn.control',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'tcn',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'isolation': {
    'specs': [
      'tcn',
    ],
    'regress_args': ['--load-extension=tcn'],
  },
}
