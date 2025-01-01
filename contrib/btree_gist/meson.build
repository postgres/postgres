# Copyright (c) 2022-2025, PostgreSQL Global Development Group

btree_gist_sources = files(
  'btree_bit.c',
  'btree_bool.c',
  'btree_bytea.c',
  'btree_cash.c',
  'btree_date.c',
  'btree_enum.c',
  'btree_float4.c',
  'btree_float8.c',
  'btree_gist.c',
  'btree_inet.c',
  'btree_int2.c',
  'btree_int4.c',
  'btree_int8.c',
  'btree_interval.c',
  'btree_macaddr.c',
  'btree_macaddr8.c',
  'btree_numeric.c',
  'btree_oid.c',
  'btree_text.c',
  'btree_time.c',
  'btree_ts.c',
  'btree_utils_num.c',
  'btree_utils_var.c',
  'btree_uuid.c',
)

if host_system == 'windows'
  btree_gist_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'btree_gist',
    '--FILEDESC', 'btree_gist - B-tree equivalent GiST operator classes',])
endif

btree_gist = shared_module('btree_gist',
  btree_gist_sources,
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args,
)
contrib_targets += btree_gist

install_data(
  'btree_gist.control',
  'btree_gist--1.0--1.1.sql',
  'btree_gist--1.1--1.2.sql',
  'btree_gist--1.2.sql',
  'btree_gist--1.2--1.3.sql',
  'btree_gist--1.3--1.4.sql',
  'btree_gist--1.4--1.5.sql',
  'btree_gist--1.5--1.6.sql',
  'btree_gist--1.6--1.7.sql',
  'btree_gist--1.7--1.8.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'btree_gist',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'init',
      'int2',
      'int4',
      'int8',
      'float4',
      'float8',
      'cash',
      'oid',
      'timestamp',
      'timestamptz',
      'time',
      'timetz',
      'date',
      'interval',
      'macaddr',
      'macaddr8',
      'inet',
      'cidr',
      'text',
      'varchar',
      'char',
      'bytea',
      'bit',
      'varbit',
      'numeric',
      'uuid',
      'not_equal',
      'enum',
      'bool',
      'partitions',
      'stratnum',
      'without_overlaps',
    ],
  },
}
