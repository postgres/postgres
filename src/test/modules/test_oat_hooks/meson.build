# FIXME: prevent install during main install, but not during test :/
test_oat_hooks = shared_module('test_oat_hooks',
  ['test_oat_hooks.c'],
  kwargs: pg_mod_args,
)
testprep_targets += test_oat_hooks

tests += {
  'name': 'test_oat_hooks',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_oat_hooks',
    ],
    'regress_args': ['--no-locale', '--encoding=UTF8'],
  },
}
