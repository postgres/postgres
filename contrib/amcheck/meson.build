amcheck = shared_module('amcheck', [
    'verify_heapam.c',
    'verify_nbtree.c',
  ],
  kwargs: contrib_mod_args,
)
contrib_targets += amcheck

install_data(
  'amcheck.control',
  'amcheck--1.0.sql',
  'amcheck--1.0--1.1.sql',
  'amcheck--1.1--1.2.sql',
  'amcheck--1.2--1.3.sql',
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
      'check_heap',
    ],
  },
  'tap': {
    'tests': [
      't/001_verify_heapam.pl',
      't/002_cic.pl',
      't/003_cic_2pc.pl',
    ],
  },
}

