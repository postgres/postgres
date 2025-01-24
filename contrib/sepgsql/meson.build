# Copyright (c) 2022-2025, PostgreSQL Global Development Group

if not selinux.found()
  subdir_done()
endif

sepgsql_sources = files(
  'database.c',
  'dml.c',
  'hooks.c',
  'label.c',
  'proc.c',
  'relation.c',
  'schema.c',
  'selinux.c',
  'uavc.c',
)

if host_system == 'windows'
  sepgsql_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'sepgsql',
    '--FILEDESC', 'sepgsql - SELinux integration',])
endif

sepgsql = shared_module('sepgsql',
  sepgsql_sources,
  c_pch: pch_postgres_h,
  kwargs: contrib_mod_args + {
    'dependencies': [selinux, contrib_mod_args['dependencies']],
  }
)
contrib_targets += sepgsql

contrib_targets += custom_target('sepgsql.sql',
  input: 'sepgsql.sql.in',
  output: 'sepgsql.sql',
  command: [sed, '-e', 's,MODULE_PATHNAME,$libdir/sepgsql,g', '@INPUT@'],
  capture: true,
  install: true,
  install_dir: dir_data / 'contrib',
)

tests += {
  'name': 'sepgsql',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_sepgsql.pl',
    ],
  },
}
