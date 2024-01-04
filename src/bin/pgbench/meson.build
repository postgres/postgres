# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pgbench_sources = files(
  'pgbench.c',
)

exprscan = custom_target('exprscan',
  input: 'exprscan.l',
  output: 'exprscan.c',
  command: flex_cmd,
)
generated_sources += exprscan
pgbench_sources += exprscan

exprparse = custom_target('exprparse',
  input: 'exprparse.y',
  kwargs: bison_kw,
)
generated_sources += exprparse.to_list()
pgbench_sources += exprparse

if host_system == 'windows'
  pgbench_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pgbench',
    '--FILEDESC', 'pgbench - a simple program for running benchmark tests'])
endif

pgbench = executable('pgbench',
  pgbench_sources,
  dependencies: [frontend_code, libpq, thread_dep],
  include_directories: include_directories('.'),
  c_pch: pch_postgres_fe_h,
  c_args: host_system == 'windows' ? ['-DFD_SETSIZE=1024'] : [],
  kwargs: default_bin_args,
)
bin_targets += pgbench

tests += {
  'name': 'pgbench',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_pgbench_with_server.pl',
      't/002_pgbench_no_server.pl',
    ],
  },
}
