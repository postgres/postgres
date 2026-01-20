# Copyright (c) 2025-2026, PostgreSQL Global Development Group

if not have_cxx
  subdir_done()
endif

# Currently not supported, to be fixed.
if cc.get_id() == 'msvc'
  subdir_done()
endif

test_cplusplusext_sources = files(
  'test_cplusplusext.cpp',
)

if host_system == 'windows'
  test_cplusplusext_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_cplusplusext',
    '--FILEDESC', 'test_cplusplusext - test C++ compatibility of PostgreSQL headers',])
endif

test_cplusplusext = shared_module('test_cplusplusext',
  test_cplusplusext_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_cplusplusext

test_install_data += files(
  'test_cplusplusext.control',
  'test_cplusplusext--1.0.sql',
)

tests += {
  'name': 'test_cplusplusext',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_cplusplusext',
    ],
  },
}
