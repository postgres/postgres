# Copyright (c) 2025-2026, PostgreSQL Global Development Group

test_custom_var_stats_sources = files(
  'test_custom_var_stats.c',
)

if host_system == 'windows'
  test_custom_var_stats_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_custom_var_stats',
    '--FILEDESC', 'test_custom_var_stats - variable-sized custom pgstats',])
endif

test_custom_var_stats = shared_module('test_custom_var_stats',
  test_custom_var_stats_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_custom_var_stats

test_install_data += files(
  'test_custom_var_stats.control',
  'test_custom_var_stats--1.0.sql',
)

test_custom_fixed_stats_sources = files(
  'test_custom_fixed_stats.c',
)

if host_system == 'windows'
  test_custom_fixed_stats_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_custom_fixed_stats',
    '--FILEDESC', 'test_custom_fixed_stats - fixed-sized custom pgstats',])
endif

test_custom_fixed_stats = shared_module('test_custom_fixed_stats',
  test_custom_fixed_stats_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_custom_fixed_stats

test_install_data += files(
  'test_custom_fixed_stats.control',
  'test_custom_fixed_stats--1.0.sql',
)

tests += {
  'name': 'test_custom_stats',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_custom_stats.pl',
    ],
    'runningcheck': false,
  },
}
