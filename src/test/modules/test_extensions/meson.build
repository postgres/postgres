# Copyright (c) 2022-2026, PostgreSQL Global Development Group

test_install_data += files(
  'test_ext1--1.0.sql',
  'test_ext1.control',
  'test_ext2--1.0.sql',
  'test_ext2.control',
  'test_ext3--1.0.sql',
  'test_ext3.control',
  'test_ext4--1.0.sql',
  'test_ext4.control',
  'test_ext5--1.0.sql',
  'test_ext5.control',
  'test_ext6--1.0.sql',
  'test_ext6.control',
  'test_ext7--1.0--2.0.sql',
  'test_ext7--1.0.sql',
  'test_ext7--2.0--2.1bad.sql',
  'test_ext7--2.0--2.2bad.sql',
  'test_ext7.control',
  'test_ext8--1.0.sql',
  'test_ext8.control',
  'test_ext9--1.0.sql',
  'test_ext9.control',
  'test_ext_cine--1.0.sql',
  'test_ext_cine--1.0--1.1.sql',
  'test_ext_cine.control',
  'test_ext_cor--1.0.sql',
  'test_ext_cor.control',
  'test_ext_cyclic1--1.0.sql',
  'test_ext_cyclic1.control',
  'test_ext_cyclic2--1.0.sql',
  'test_ext_cyclic2.control',
  'test_ext_extschema--1.0.sql',
  'test_ext_extschema.control',
  'test_ext_evttrig--1.0--2.0.sql',
  'test_ext_evttrig--1.0.sql',
  'test_ext_evttrig.control',
  'test_ext_req_schema1--1.0.sql',
  'test_ext_req_schema1.control',
  'test_ext_req_schema2--1.0.sql',
  'test_ext_req_schema2.control',
  'test_ext_req_schema3--1.0.sql',
  'test_ext_req_schema3.control',
  'test_ext_set_schema--1.0.sql',
  'test_ext_set_schema.control',
)

test_ext_sources = files('test_ext.c')

if host_system == 'windows'
  test_ext_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_ext',
    '--FILEDESC', 'test_ext - test C extension for pg_upgrade',])
endif

test_ext = shared_module('test_ext',
  test_ext_sources,
  kwargs: pg_test_mod_args,
)

tests += {
  'name': 'test_extensions',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_extensions',
      'test_extdepend',
    ],
    'regress_args': ['--no-locale'],
  },
  'tap': {
    'tests': [
      't/001_extension_control_path.pl',
    ],
  },
}
