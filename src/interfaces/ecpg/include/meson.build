# Copyright (c) 2022-2024, PostgreSQL Global Development Group

ecpg_inc = include_directories('.')

ecpg_conf_keys = [
  'SIZEOF_LONG',
  'SIZEOF_LONG_LONG',
]

ecpg_conf_data = configuration_data()

ecpg_conf_data.set('ENABLE_THREAD_SAFETY', 1)

foreach key : ecpg_conf_keys
  if cdata.has(key)
    ecpg_conf_data.set(key, cdata.get(key))
  endif
endforeach

ecpg_config_h = configure_file(
  output: 'ecpg_config.h',
  configuration: ecpg_conf_data,
  install_dir: dir_include,
)
configure_files += ecpg_config_h

generated_sources_ac += {'src/interfaces/ecpg/include': ['stamp-h']}

install_headers(
  'ecpg_informix.h',
  'ecpgerrno.h',
  'ecpglib.h',
  'ecpgtype.h',
  'pgtypes.h',
  'pgtypes_date.h',
  'pgtypes_error.h',
  'pgtypes_interval.h',
  'pgtypes_numeric.h',
  'pgtypes_timestamp.h',
  'sql3types.h',
  'sqlca.h',
  'sqlda.h',
  'sqlda-compat.h',
  'sqlda-native.h',
)

install_headers(
  'datetime.h',
  'decimal.h',
  'sqltypes.h',
  install_dir: dir_include_pkg / 'informix' / 'esql',
)
