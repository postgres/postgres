# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pgoutput_sources = files(
  'pgoutput.c',
)

if host_system == 'windows'
  pgoutput_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pgoutput',
    '--FILEDESC', 'pgoutput - standard logical replication output plugin',])
endif

pgoutput = shared_module('pgoutput',
  pgoutput_sources,
  kwargs: pg_mod_args,
)

backend_targets += pgoutput
