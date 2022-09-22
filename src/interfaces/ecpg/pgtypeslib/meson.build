ecpg_pgtypes_sources = files(
  'common.c',
  'datetime.c',
  'dt_common.c',
  'interval.c',
  'numeric.c',
  'timestamp.c',
)

export_file = custom_target('libpq.exports', kwargs: gen_export_kwargs)

ecpg_pgtypes = both_libraries('libpgtypes',
  ecpg_pgtypes_sources,
  include_directories: ['.', ecpg_inc, postgres_inc],
  c_args: ['-DSO_MAJOR_VERSION=3'],
  dependencies: [frontend_code],
  version: '3.' + pg_version_major.to_string(),
  soversion: host_system != 'windows' ? '3' : '',
  darwin_versions: ['3', '3.' + pg_version_major.to_string()],
  link_args: export_fmt.format(export_file.full_path()),
  link_depends: export_file,
  kwargs: default_lib_args,
)
ecpg_targets += [ecpg_pgtypes.get_shared_lib(), ecpg_pgtypes.get_static_lib()]

pkgconfig.generate(
  ecpg_pgtypes.get_shared_lib(),
  description: 'PostgreSQL libpgtypes library',
  url: pg_url,
)
