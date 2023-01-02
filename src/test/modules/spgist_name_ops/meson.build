# Copyright (c) 2022-2023, PostgreSQL Global Development Group

# FIXME: prevent install during main install, but not during test :/

spgist_name_ops_sources = files(
  'spgist_name_ops.c',
)

if host_system == 'windows'
  spgist_name_ops_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'spgist_name_ops',
    '--FILEDESC', 'spgist_name_ops - test opclass for SP-GiST',])
endif

spgist_name_ops = shared_module('spgist_name_ops',
  spgist_name_ops_sources,
  kwargs: pg_mod_args,
)
testprep_targets += spgist_name_ops

install_data(
  'spgist_name_ops.control',
  'spgist_name_ops--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'spgist_name_ops',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'spgist_name_ops',
    ],
  },
}
