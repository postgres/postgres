# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pg_config_ext = configure_file(
  input: 'pg_config_ext.h.meson',
  output: 'pg_config_ext.h',
  configuration: cdata,
  install: true,
  install_dir: dir_include,
)
configure_files += pg_config_ext

pg_config_os = configure_file(
  output: 'pg_config_os.h',
  input: files('port/@0@.h'.format(portname)),
  install: true,
  install_dir: dir_include,
  copy: true,
)
configure_files += pg_config_os

pg_config = configure_file(
  output: 'pg_config.h',
  install: true,
  install_dir: dir_include,
  configuration: cdata,
)
configure_files += pg_config


config_paths_data = configuration_data()
config_paths_data.set_quoted('PGBINDIR', dir_prefix / dir_bin)
config_paths_data.set_quoted('PGSHAREDIR', dir_prefix / dir_data)
config_paths_data.set_quoted('SYSCONFDIR', dir_prefix / dir_sysconf)
config_paths_data.set_quoted('INCLUDEDIR', dir_prefix / dir_include)
config_paths_data.set_quoted('PKGINCLUDEDIR', dir_prefix / dir_include_pkg)
config_paths_data.set_quoted('INCLUDEDIRSERVER', dir_prefix / dir_include_server)
config_paths_data.set_quoted('LIBDIR', dir_prefix / dir_lib)
config_paths_data.set_quoted('PKGLIBDIR', dir_prefix / dir_lib_pkg)
config_paths_data.set_quoted('LOCALEDIR', dir_prefix / dir_locale)
config_paths_data.set_quoted('DOCDIR', dir_prefix / dir_doc)
config_paths_data.set_quoted('HTMLDIR', dir_prefix / dir_doc_html)
config_paths_data.set_quoted('MANDIR', dir_prefix / dir_man)


var_cc = ' '.join(cc.cmd_array())
var_cpp = ' '.join(cc.cmd_array() + ['-E'])
var_cflags = ' '.join(cflags + cflags_builtin + cflags_warn + get_option('c_args'))
if llvm.found()
  var_cxxflags = ' '.join(cxxflags + cxxflags_builtin + cxxflags_warn + get_option('cpp_args'))
else
  var_cxxflags = ''
endif
var_cppflags = ' '.join(cppflags)
var_cflags_sl = ' '.join(cc.get_supported_arguments('-fPIC'))
# explicitly add -Wl,--as-needed, normally added by meson, but we want it for
# PGXS compatibility
var_ldflags = ' '.join(
  ldflags
  + cc.get_supported_link_arguments('-Wl,--as-needed')
  + get_option('c_link_args')
)
var_ldflags_sl = ''.join(ldflags_sl)
var_ldflags_ex = '' # FIXME
# FIXME - some extensions might directly use symbols from one of libs. If
# that symbol isn't used by postgres, and statically linked, it'll cause an
# undefined symbol at runtime. And obviously it'll cause problems for
# executables, although those are probably less common.
var_libs = ''


pg_config_paths = configure_file(
  output: 'pg_config_paths.h',
  configuration: config_paths_data,
  install: false,
)
configure_files += pg_config_paths

install_headers(
  'pg_config_manual.h',
  'postgres_ext.h',
)

install_headers(
  'libpq/libpq-fs.h',
  install_dir: dir_include / 'libpq',
)

install_headers(
  'c.h',
  'port.h',
  'postgres_fe.h',
  install_dir: dir_include_internal,
)

install_headers(
  'libpq/pqcomm.h',
  'libpq/protocol.h',
  install_dir: dir_include_internal / 'libpq',
)

install_headers(
  'c.h',
  'fmgr.h',
  'funcapi.h',
  'getopt_long.h',
  'miscadmin.h',
  'pg_config_manual.h',
  'pg_getopt.h',
  'pg_trace.h',
  'pgstat.h',
  'pgtar.h',
  'pgtime.h',
  'port.h',
  'postgres.h',
  'postgres_ext.h',
  'postgres_fe.h',
  'varatt.h',
  'windowapi.h',
  pg_config_ext,
  pg_config_os,
  pg_config,
  install_dir: dir_include_server,
)

subdir('catalog')
subdir('nodes')
subdir('pch')
subdir('storage')
subdir('utils')

header_subdirs = [
  'access',
  'archive',
  'catalog',
  'bootstrap',
  'commands',
  'common',
  'datatype',
  'executor',
  'fe_utils',
  'foreign',
  'jit',
  'lib',
  'libpq',
  'mb',
  'nodes',
  'optimizer',
  'parser',
  'partitioning',
  'postmaster',
  'regex',
  'replication',
  'rewrite',
  'statistics',
  'storage',
  'tcop',
  'snowball',
  'tsearch',
  'utils',
  'port',
  'portability',
]

# XXX: installing headers this way has the danger of installing editor files
# etc, unfortunately install_subdir() doesn't allow including / excluding by
# pattern currently.
foreach d : header_subdirs
  if d == 'catalog'
    continue
  endif
  install_subdir(d, install_dir: dir_include_server,
                 exclude_files: ['.gitignore', 'meson.build'])
endforeach

install_subdir('catalog',
  install_dir: dir_include_server,
  exclude_files: [
    '.gitignore',
    'Makefile',
    'duplicate_oids',
    'meson.build',
    'reformat_dat_file.pl',
    'renumber_oids.pl',
    'unused_oids',
  ] + bki_data,
)

# autoconf generates the file there, ensure we get a conflict
generated_sources_ac += {'src/include': ['stamp-h', 'stamp-ext-h']}
