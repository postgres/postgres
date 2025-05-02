# Copyright (c) 2022-2025, PostgreSQL Global Development Group

if not oauth_flow_supported
  subdir_done()
endif

libpq_oauth_sources = files(
  'oauth-curl.c',
)

# The shared library needs additional glue symbols.
libpq_oauth_so_sources = files(
  'oauth-utils.c',
)
libpq_oauth_so_c_args = ['-DUSE_DYNAMIC_OAUTH']

export_file = custom_target('libpq-oauth.exports',
  kwargs: gen_export_kwargs,
)

# port needs to be in include path due to pthread-win32.h
libpq_oauth_inc = include_directories('.', '../libpq', '../../port')

libpq_oauth_st = static_library('libpq-oauth',
  libpq_oauth_sources,
  include_directories: [libpq_oauth_inc, postgres_inc],
  c_pch: pch_postgres_fe_h,
  dependencies: [
    frontend_stlib_code,
    libpq_oauth_deps,
    ssl, # libpq-int.h includes OpenSSL headers
  ],
  kwargs: default_lib_args,
)

# This is an internal module; we don't want an SONAME and therefore do not set
# SO_MAJOR_VERSION.
libpq_oauth_name = 'libpq-oauth-@0@'.format(pg_version_major)

libpq_oauth_so = shared_module(libpq_oauth_name,
  libpq_oauth_sources + libpq_oauth_so_sources,
  include_directories: [libpq_oauth_inc, postgres_inc],
  c_args: libpq_so_c_args,
  c_pch: pch_postgres_fe_h,
  dependencies: [frontend_shlib_code, libpq, libpq_oauth_deps],
  link_depends: export_file,
  link_args: export_fmt.format(export_file.full_path()),
  kwargs: default_lib_args,
)
