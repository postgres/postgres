btree_gin = shared_module('btree_gin',
  files('btree_gin.c'),
  kwargs: contrib_mod_args,
)
contrib_targets += btree_gin

install_data(
  'btree_gin.control',
  'btree_gin--1.0.sql',
  'btree_gin--1.0--1.1.sql',
  'btree_gin--1.1--1.2.sql',
  'btree_gin--1.2--1.3.sql',
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
