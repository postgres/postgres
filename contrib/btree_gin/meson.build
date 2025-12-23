# Copyright (c) 2022-2025, PostgreSQL Global Development Group

btree_gin_sources = files(
  'btree_gin.c',
)

if host_system == 'windows'
  btree_gin_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'btree_gin',
    '--FILEDESC', 'btree_gin - B-tree equivalent GIN operator classes',])
endif

btree_gin = shared_module('btree_gin',
  btree_gin_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += btree_gin

install_data(
  'btree_gin.control',
  'btree_gin--1.0.sql',
  'btree_gin--1.0--1.1.sql',
  'btree_gin--1.1--1.2.sql',
  'btree_gin--1.2--1.3.sql',
  'btree_gin--1.3--1.4.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'btree_gin',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'install_btree_gin',
      'int2',
      'int4',
      'int8',
      'float4',
      'float8',
      'money',
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
      'enum',
      'uuid',
      'name',
      'bool',
      'bpchar',
    ],
  },
}
