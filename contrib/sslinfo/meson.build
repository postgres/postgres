# Copyright (c) 2022-2024, PostgreSQL Global Development Group

if not ssl.found()
  subdir_done()
endif

sslinfo_sources = files(
  'sslinfo.c',
)

if host_system == 'windows'
  sslinfo_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'sslinfo',
    '--FILEDESC', 'sslinfo - information about client SSL certificate',])
endif

sslinfo = shared_module('sslinfo',
  sslinfo_sources,
  kwargs: contrib_mod_args + {
    'dependencies': [ssl, contrib_mod_args['dependencies']],
  }
)
contrib_targets += sslinfo

install_data(
  'sslinfo--1.0--1.1.sql',
  'sslinfo--1.1--1.2.sql',
  'sslinfo--1.2.sql',
  'sslinfo.control',
  kwargs: contrib_data_args,
)
