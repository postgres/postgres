# Copyright (c) 2022-2026, PostgreSQL Global Development Group

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
  c_args: libpq_oauth_so_c_args,
  c_pch: pch_postgres_fe_h,
  dependencies: [frontend_shlib_code, libpq, libpq_oauth_deps],
  link_depends: export_file,
  link_args: export_fmt.format(export_file.full_path()),
  kwargs: default_lib_args,
)

libpq_oauth_test_deps = []

oauth_test_sources = files('test-oauth-curl.c') + libpq_oauth_so_sources

if host_system == 'windows'
  oauth_test_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'oauth_tests',
    '--FILEDESC', 'OAuth unit test program',])
endif

libpq_oauth_test_deps += executable('oauth_tests',
  oauth_test_sources,
  dependencies: [frontend_shlib_code, libpq, libpq_oauth_deps],
  kwargs: default_bin_args + {
    'c_args': default_bin_args.get('c_args', []) + libpq_oauth_so_c_args,
    'c_pch': pch_postgres_fe_h,
    'include_directories': [libpq_inc, postgres_inc],
    'install': false,
  }
)

testprep_targets += libpq_oauth_test_deps

tests += {
  'name': 'libpq-oauth',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_oauth.pl',
    ],
    'deps': libpq_oauth_test_deps,
  },
}
