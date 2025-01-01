# Copyright (c) 2022-2025, PostgreSQL Global Development Group

dummy_seclabel_sources = files(
  'dummy_seclabel.c',
)

if host_system == 'windows'
  dummy_seclabel_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'dummy_seclabel',
    '--FILEDESC', 'dummy_seclabel - regression testing of the SECURITY LABEL statement',])
endif

dummy_seclabel = shared_module('dummy_seclabel',
  dummy_seclabel_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += dummy_seclabel

test_install_data += files(
  'dummy_seclabel.control',
  'dummy_seclabel--1.0.sql',
)

tests += {
  'name': 'dummy_seclabel',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'dummy_seclabel',
    ],
  },
}
