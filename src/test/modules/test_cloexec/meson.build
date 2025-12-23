# src/test/modules/test_cloexec/meson.build

# This test is Windows-only
if host_system != 'windows'
  subdir_done()
endif

test_cloexec_sources = files('test_cloexec.c')

test_cloexec = executable('test_cloexec',
  test_cloexec_sources,
  dependencies: [frontend_code],
  link_with: [pgport[''], pgcommon['']],
  kwargs: default_bin_args,
)

tests += {
  'name': 'test_cloexec',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_cloexec.pl',
    ],
  },
}
