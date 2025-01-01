# Copyright (c) 2022-2025, PostgreSQL Global Development Group

auto_explain_sources = files(
  'auto_explain.c',
)

if host_system == 'windows'
  auto_explain_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'auto_explain',
    '--FILEDESC', 'auto_explain - logging facility for execution plans',])
endif

auto_explain = shared_module('auto_explain',
  auto_explain_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += auto_explain

tests += {
  'name': 'auto_explain',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_auto_explain.pl',
    ],
  },
}
