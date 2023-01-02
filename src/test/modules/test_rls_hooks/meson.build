# Copyright (c) 2022-2023, PostgreSQL Global Development Group

# FIXME: prevent install during main install, but not during test :/

test_rls_hooks_sources = files(
  'test_rls_hooks.c',
)

if host_system == 'windows'
  test_rls_hooks_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_rls_hooks',
    '--FILEDESC', 'test_rls_hooks - example use of RLS hooks',])
endif

test_rls_hooks = shared_module('test_rls_hooks',
  test_rls_hooks_sources,
  kwargs: pg_mod_args,
)
testprep_targets += test_rls_hooks

tests += {
  'name': 'test_rls_hooks',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_rls_hooks',
    ],
  },
}
