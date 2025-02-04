# Copyright (c) 2024-2025, PostgreSQL Global Development Group

test_json_parser_incremental_sources = files(
  'test_json_parser_incremental.c',
)

if host_system == 'windows'
  test_json_parser_incremental_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_json_parser_incremental',
    '--FILEDESC', 'standalone json parser tester',
  ])
endif

test_json_parser_incremental = executable('test_json_parser_incremental',
  test_json_parser_incremental_sources,
  dependencies: [frontend_code],
  kwargs: default_bin_args + {
    'install': false,
  },
)

# A second version of test_json_parser_incremental, this time compiled against
# the shared-library flavor of jsonapi.
test_json_parser_incremental_shlib = executable('test_json_parser_incremental_shlib',
  test_json_parser_incremental_sources,
  dependencies: [frontend_shlib_code, libpq],
  c_args: ['-DJSONAPI_SHLIB_ALLOC'],
  link_with: [common_excluded_shlib],
  kwargs: default_bin_args + {
    'install': false,
  },
)

test_json_parser_perf_sources = files(
  'test_json_parser_perf.c',
)

if host_system == 'windows'
  test_json_parser_perf_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_json_parser_perf',
    '--FILEDESC', 'standalone json parser tester',
  ])
endif

test_json_parser_perf = executable('test_json_parser_perf',
  test_json_parser_perf_sources,
  dependencies: [frontend_code],
  kwargs: default_bin_args + {
    'install': false,
  },
)

tests += {
  'name': 'test_json_parser',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_test_json_parser_incremental.pl',
      't/002_inline.pl',
      't/003_test_semantic.pl',
      't/004_test_parser_perf.pl'
    ],
    'deps': [
      test_json_parser_incremental,
      test_json_parser_incremental_shlib,
      test_json_parser_perf,
    ],
  },
}
