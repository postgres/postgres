# Copyright (c) 2022-2025, PostgreSQL Global Development Group

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
ecpglib_so_sources = [] # for shared lib, in addition to the above

ecpglib_inc = [include_directories('.'), ecpg_inc]
ecpglib_c_args = ['-DSO_MAJOR_VERSION=6']
export_file = custom_target('libecpg.exports', kwargs: gen_export_kwargs)

if host_system == 'windows'
  ecpglib_so_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'libecpg',
    '--FILEDESC', 'ECPG - embedded SQL in C',])
endif

# see src/interfaces/libpq/meson.build
ecpglib_st = static_library('libecpg',
  ecpglib_sources,
  include_directories: ecpglib_inc,
  c_args: ecpglib_c_args,
  c_pch: pch_postgres_fe_h,
  dependencies: [frontend_stlib_code, thread_dep, libpq],
  link_with: [ecpg_pgtypes_st],
  kwargs: default_lib_args,
)
ecpg_targets += ecpglib_st

ecpglib_so = shared_library('libecpg',
  ecpglib_sources + ecpglib_so_sources,
  include_directories: ecpglib_inc,
  c_args: ecpglib_c_args,
  c_pch: pch_postgres_fe_h,
  dependencies: [frontend_shlib_code, libpq, thread_dep],
  link_with: ecpg_pgtypes_so,
  soversion: host_system != 'windows' ? '6' : '',
  darwin_versions: ['6', '6.' + pg_version_major.to_string()],
  version: '6.' + pg_version_major.to_string(),
  link_args: export_fmt.format(export_file.full_path()),
  link_depends: export_file,
  kwargs: default_lib_args,
)
ecpg_targets += ecpglib_so

pkgconfig.generate(
  name: 'libecpg',
  description: 'PostgreSQL libecpg library',
  url: pg_url,
  libraries: ecpglib_so,
  libraries_private: [frontend_stlib_code, thread_dep],
  requires_private: ['libpgtypes', 'libpq'],
)

subdir('po', if_found: libintl)
