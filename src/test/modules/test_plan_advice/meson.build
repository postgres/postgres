# Copyright (c) 2022-2026, PostgreSQL Global Development Group

test_plan_advice_sources = files(
  'test_plan_advice.c',
)

if host_system == 'windows'
  test_plan_advice_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_plan_advice',
    '--FILEDESC', 'test_plan_advice - test whether generated plan advice works',])
endif

test_plan_advice = shared_module('test_plan_advice',
  test_plan_advice_sources,
  include_directories: pg_plan_advice_inc,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_plan_advice

tests += {
  'name': 'test_plan_advice',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_replan_regress.pl',
    ],
  },
}
