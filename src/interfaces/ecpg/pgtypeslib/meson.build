# Copyright (c) 2022-2024, PostgreSQL Global Development Group

ecpg_pgtypes_sources = files(
  'common.c',
  'datetime.c',
  'dt_common.c',
  'interval.c',
  'numeric.c',
  'timestamp.c',
)
ecpg_pgtypes_so_sources = [] # for shared lib, in addition to the above

export_file = custom_target('libpgtypes.exports', kwargs: gen_export_kwargs)
ecpg_pgtypes_inc = [include_directories('.'), ecpg_inc]
ecpg_pgtypes_c_args = ['-DSO_MAJOR_VERSION=3']

if host_system == 'windows'
  ecpg_pgtypes_so_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pgtypes',
    '--FILEDESC', 'pgtypes - library for data type mapping',])
endif

# see src/interfaces/libpq/meson.build
ecpg_pgtypes_st = static_library('libpgtypes',
  ecpg_pgtypes_sources,
  include_directories: ecpg_pgtypes_inc,
  c_args: ecpg_pgtypes_c_args,
  c_pch: pch_postgres_fe_h,
  dependencies: frontend_stlib_code,
  kwargs: default_lib_args,
)
ecpg_targets += ecpg_pgtypes_st

ecpg_pgtypes_so = shared_library('libpgtypes',
  ecpg_pgtypes_sources + ecpg_pgtypes_so_sources,
  include_directories: ecpg_pgtypes_inc,
  c_args: ecpg_pgtypes_c_args,
  c_pch: pch_postgres_fe_h,
  dependencies: frontend_shlib_code,
  version: '3.' + pg_version_major.to_string(),
  soversion: host_system != 'windows' ? '3' : '',
  darwin_versions: ['3', '3.' + pg_version_major.to_string()],
  link_args: export_fmt.format(export_file.full_path()),
  link_depends: export_file,
  kwargs: default_lib_args,
)
ecpg_targets += ecpg_pgtypes_so

pkgconfig.generate(
  name: 'libpgtypes',
  description: 'PostgreSQL libpgtypes library',
  url: pg_url,
  libraries: ecpg_pgtypes_so,
  libraries_private: [frontend_stlib_code],
)
