export_file = custom_target('libpq.exports', kwargs: gen_export_kwargs)

ecpg_compat = both_libraries('libecpg_compat',
  'informix.c',
  include_directories: ['.', ecpg_inc, postgres_inc, libpq_inc],
  c_args: ['-DSO_MAJOR_VERSION=3'],
  dependencies: [frontend_code, thread_dep],
  link_with: [ecpglib, ecpg_pgtypes],
  soversion: host_system != 'windows' ? '3' : '',
  darwin_versions: ['3', '3.' + pg_version_major.to_string()],
  version: '3.' + pg_version_major.to_string(),
  link_args: export_fmt.format(export_file.full_path()),
  link_depends: export_file,
  kwargs: default_lib_args,
)
ecpg_targets += [ecpg_compat.get_shared_lib(), ecpg_compat.get_static_lib()]

pkgconfig.generate(
  ecpg_compat.get_shared_lib(),
  description: 'PostgreSQL libecpg_compat library',
  url: pg_url,
)
