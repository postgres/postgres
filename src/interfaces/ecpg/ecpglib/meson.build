ecpglib_sources = files(
  'connect.c',
  'data.c',
  'descriptor.c',
  'error.c',
  'execute.c',
  'memory.c',
  'misc.c',
  'prepare.c',
  'sqlda.c',
  'typename.c',
)

export_file = custom_target('libpq.exports', kwargs: gen_export_kwargs)

ecpglib = both_libraries('libecpg',
  ecpglib_sources,
  include_directories: ['.', ecpg_inc, postgres_inc],
  c_args: ['-DSO_MAJOR_VERSION=6'],
  dependencies: [frontend_code, libpq, thread_dep],
  link_with: [ecpg_pgtypes],
  soversion: host_system != 'windows' ? '6' : '',
  darwin_versions: ['6', '6.' + pg_version_major.to_string()],
  version: '6.' + pg_version_major.to_string(),
  link_args: export_fmt.format(export_file.full_path()),
  link_depends: export_file,
  kwargs: default_lib_args,
)
ecpg_targets += [ecpglib.get_shared_lib(), ecpglib.get_static_lib()]

pkgconfig.generate(
  ecpglib.get_shared_lib(),
  description: 'PostgreSQL libecpg library',
  url: pg_url,
)

subdir('po', if_found: libintl)
