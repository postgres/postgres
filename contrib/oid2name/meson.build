# Copyright (c) 2022-2024, PostgreSQL Global Development Group

oid2name_sources = files(
  'oid2name.c',
)

if host_system == 'windows'
  oid2name_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'oid2name',
    '--FILEDESC', 'oid2name - examine the file structure',])
endif

oid2name = executable('oid2name',
  oid2name_sources,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args,
)
contrib_targets += oid2name

tests += {
  'name': 'oid2name',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_basic.pl',
    ],
  },
}
