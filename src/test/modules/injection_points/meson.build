# Copyright (c) 2022-2025, PostgreSQL Global Development Group

if not get_option('injection_points')
  subdir_done()
endif

injection_points_sources = files(
  'injection_points.c',
  'injection_stats.c',
  'injection_stats_fixed.c',
  'regress_injection.c',
)

if host_system == 'windows'
  injection_points_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'injection_points',
    '--FILEDESC', 'injection_points - facility for injection points',])
endif

injection_points = shared_module('injection_points',
  injection_points_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += injection_points

test_install_data += files(
  'injection_points.control',
  'injection_points--1.0.sql',
)

tests += {
  'name': 'injection_points',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'injection_points',
      'hashagg',
      'reindex_conc',
    ],
    'regress_args': ['--dlpath', meson.build_root() / 'src/test/regress'],
    # The injection points are cluster-wide, so disable installcheck
    'runningcheck': false,
  },
  'isolation': {
    'specs': [
      'basic',
      'inplace',
      'syscache-update-pruned',
    ],
    'runningcheck': false, # see syscache-update-pruned
  },
  'tap': {
    'env': {
      'enable_injection_points': get_option('injection_points') ? 'yes' : 'no',
    },
    'tests': [
      't/001_stats.pl',
    ],
  },
}
