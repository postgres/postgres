# Copyright (c) 2026, PostgreSQL Global Development Group

pgrepack_sources = files(
  'pgrepack.c',
)

if host_system == 'windows'
  pgrepack_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pgrepack',
    '--FILEDESC', 'pgrepack - logical replication output plugin for REPACK',])
endif

pgrepack = shared_module('pgrepack',
  pgrepack_sources,
  kwargs: pg_mod_args,
)

backend_targets += pgrepack
