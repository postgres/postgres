# Copyright (c) 2022-2024, PostgreSQL Global Development Group

# pg_regress_c helpfully provided by regress/meson.build

isolation_sources = pg_regress_c + files(
  'isolation_main.c',
)

isolationtester_sources = files(
  'isolationtester.c',
)

spec_scanner = custom_target('specscanner',
  input: 'specscanner.l',
  output: 'specscanner.c',
  command: flex_cmd,
)
isolationtester_sources += spec_scanner
generated_sources += spec_scanner

spec_parser = custom_target('specparse',
  input: 'specparse.y',
  kwargs: bison_kw,
)
isolationtester_sources += spec_parser
generated_sources += spec_parser.to_list()

if host_system == 'windows'
  isolation_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_isolation_regress',
    '--FILEDESC', 'pg_isolation_regress - multi-client test driver',])
endif

pg_isolation_regress = executable('pg_isolation_regress',
  isolation_sources,
  c_args: pg_regress_cflags,
  include_directories: pg_regress_inc,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install_dir': dir_pgxs / 'src/test/isolation',
  },
)
bin_targets += pg_isolation_regress


if host_system == 'windows'
  isolationtester_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'isolationtester',
    '--FILEDESC', 'isolationtester - multi-client test driver',])
endif

isolationtester = executable('isolationtester',
  isolationtester_sources,
  include_directories: include_directories('.'),
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install_dir': dir_pgxs / 'src/test/isolation',
  },
)
bin_targets += isolationtester

tests += {
  'name': 'isolation',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'isolation': {
    'schedule': files('isolation_schedule'),
    'test_kwargs': {
      'priority': 40,
      'timeout': 1000,
    },
    'dbname': 'isolation_regression',
  },
}
