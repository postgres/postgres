# test/ is entered via top-level meson.build, that way it can use the default
# args for executables (which depend on libpq).

libpq_sources = files(
  'fe-auth-scram.c',
  'fe-auth.c',
  'fe-connect.c',
  'fe-exec.c',
  'fe-lobj.c',
  'fe-misc.c',
  'fe-print.c',
  'fe-protocol3.c',
  'fe-secure.c',
  'fe-trace.c',
  'legacy-pqsignal.c',
  'libpq-events.c',
  'pqexpbuffer.c',
)

if host_system == 'windows'
  libpq_sources += files('pthread-win32.c', 'win32.c')
endif

if ssl.found()
  libpq_sources += files('fe-secure-common.c')
  libpq_sources += files('fe-secure-openssl.c')
endif

if gssapi.found()
  libpq_sources += files(
    'fe-gssapi-common.c',
    'fe-secure-gssapi.c',
  )
endif

export_file = custom_target('libpq.exports',
  kwargs: gen_export_kwargs,
)

# port needs to be in include path due to pthread-win32.h
libpq_inc = include_directories('.', '../../port')

libpq_st = static_library('libpq',
  libpq_sources,
  pic: true,
  include_directories: [libpq_inc, postgres_inc],
  c_args: ['-DSO_MAJOR_VERSION=5'],
  dependencies: libpq_deps,
  kwargs: default_lib_args,
)

# not using both_libraries here, causes problems with precompiled headers and
# resource files with msbuild
libpq_so = shared_library('libpq',
  dependencies: libpq_deps,
  include_directories: [libpq_inc, postgres_inc],
  c_args: ['-DSO_MAJOR_VERSION=5'],
  link_whole: libpq_st,
  version: '5.' + pg_version_major.to_string(),
  soversion: host_system != 'windows' ? '5' : '',
  darwin_versions: ['5', '5.' + pg_version_major.to_string()],
  link_depends: export_file,
  link_args: export_fmt.format(export_file.full_path()),
  kwargs: default_lib_args,
)

libpq = declare_dependency(
  link_with: [libpq_so],
  include_directories: [include_directories('.')]
)

pkgconfig.generate(
  libpq_so,
  name: 'libpq',
  description: 'PostgreSQL libpq library',
  url: pg_url,
)

install_headers(
  'libpq-fe.h',
  'libpq-events.h',
)

install_headers(
  'libpq-int.h',
  'pqexpbuffer.h',
  'fe-auth-sasl.h',
  install_dir: dir_include_internal,
)
install_data('pg_service.conf.sample',
  install_dir: dir_data,
)


tests += {
  'name': 'libpq',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_uri.pl',
      't/002_api.pl',
    ],
    'env': {'with_ssl': get_option('ssl')},
  },
}

subdir('po', if_found: libintl)
