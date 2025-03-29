# Copyright (c) 2022-2025, PostgreSQL Global Development Group

amcheck_sources = files(
  'verify_common.c',
  'verify_gin.c',
  'verify_heapam.c',
  'verify_nbtree.c',
)

if host_system == 'windows'
  amcheck_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'amcheck',
    '--FILEDESC', 'amcheck - function for verifying relation integrity',])
endif

amcheck = shared_module('amcheck',
  amcheck_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += amcheck

install_data(
  'amcheck.control',
  'amcheck--1.0.sql',
  'amcheck--1.0--1.1.sql',
  'amcheck--1.1--1.2.sql',
  'amcheck--1.2--1.3.sql',
  'amcheck--1.3--1.4.sql',
  'amcheck--1.4--1.5.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'amcheck',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'check',
      'check_btree',
      'check_gin',
      'check_heap',
    ],
  },
  'tap': {
    'tests': [
      't/001_verify_heapam.pl',
      't/002_cic.pl',
      't/003_cic_2pc.pl',
      't/004_verify_nbtree_unique.pl',
      't/005_pitr.pl',
    ],
  },
}
