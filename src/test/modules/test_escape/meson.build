test_escape_sources = files(
  'test_escape.c',
)

if host_system == 'windows'
  test_escape_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_escape',
    '--FILEDESC', 'test escape program',])
endif

test_escape = executable('test_escape',
  test_escape_sources,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false,
  }
)
testprep_targets += test_escape


tests += {
  'name': 'test_escape',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_test_escape.pl',
    ],
    'deps': [test_escape],
  },
}
