# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_surgery_sources = files(
  'heap_surgery.c',
)

if host_system == 'windows'
  pg_surgery_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_surgery',
    '--FILEDESC', 'pg_surgery - perform surgery on a damaged relation',])
endif

pg_surgery = shared_module('pg_surgery',
  pg_surgery_sources,
  kwargs: contrib_mod_args,
)

if llvm.found()
  contrib_targets += pg_surgery
  llvm_contrib_sources += pg_surgery_sources
endif

install_data(
  'pg_surgery--1.0.sql',
  'pg_surgery.control',
  kwargs: contrib_data_args,
)


tests += {
  'name': 'pg_surgery',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'heap_surgery',
    ],
  },
}
