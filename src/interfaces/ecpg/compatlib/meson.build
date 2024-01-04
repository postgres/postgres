# Copyright (c) 2022-2024, PostgreSQL Global Development Group

ecpg_compat_sources = files(
  'informix.c',
)
ecpg_compat_so_sources = [] # for shared lib, in addition to the above

ecpg_compat_inc = [include_directories('.'), ecpg_inc, libpq_inc]
ecpg_compat_c_args = ['-DSO_MAJOR_VERSION=3']
export_file = custom_target('libecpg_compat.exports', kwargs: gen_export_kwargs)

if host_system == 'windows'
  ecpg_compat_so_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'libecpg_compat',
    '--FILEDESC', 'ECPG compat - compatibility library for ECPG',])
endif

# see src/interfaces/libpq/meson.build
ecpg_compat_st = static_library('libecpg_compat',
  ecpg_compat_sources,
  include_directories: ecpg_compat_inc,
  c_args: ecpg_compat_c_args,
  dependencies: [frontend_stlib_code, thread_dep],
  link_with: [ecpglib_st, ecpg_pgtypes_st],
  kwargs: default_lib_args,
)
ecpg_targets += ecpg_compat_st

ecpg_compat_so = shared_library('libecpg_compat',
  ecpg_compat_sources + ecpg_compat_so_sources,
  include_directories: ecpg_compat_inc,
  c_args: ecpg_compat_c_args,
  dependencies: [frontend_shlib_code, thread_dep],
  link_with: [ecpglib_so, ecpg_pgtypes_so],
  soversion: host_system != 'windows' ? '3' : '',
  darwin_versions: ['3', '3.' + pg_version_major.to_string()],
  version: '3.' + pg_version_major.to_string(),
  link_args: export_fmt.format(export_file.full_path()),
  link_depends: export_file,
  kwargs: default_lib_args,
)
ecpg_targets += ecpg_compat_so

pkgconfig.generate(
  name: 'libecpg_compat',
  description: 'PostgreSQL libecpg_compat library',
  url: pg_url,
  libraries: ecpg_compat_so,
  libraries_private: [frontend_stlib_code, thread_dep],
  requires_private: ['libecpg', 'libpgtypes'],
)
