# Copyright (c) 2023-2025, PostgreSQL Global Development Group

xid_wraparound_sources = files(
  'xid_wraparound.c',
)

if host_system == 'windows'
  xid_wraparound_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'xid_wraparound',
    '--FILEDESC', 'xid_wraparound - tests for XID wraparound',])
endif

xid_wraparound = shared_module('xid_wraparound',
  xid_wraparound_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += xid_wraparound

test_install_data += files(
  'xid_wraparound.control',
  'xid_wraparound--1.0.sql',
)

tests += {
  'name': 'xid_wraparound',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_emergency_vacuum.pl',
      't/002_limits.pl',
      't/003_wraparounds.pl',
    ],
  },
}
