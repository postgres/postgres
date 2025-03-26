# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pg_overexplain_sources = files(
  'pg_overexplain.c',
)

if host_system == 'windows'
  pg_overexplain_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_overexplain',
    '--FILEDESC', 'pg_overexplain - allow EXPLAIN to dump even more details',])
endif

pg_overexplain = shared_module('pg_overexplain',
  pg_overexplain_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += pg_overexplain

tests += {
  'name': 'pg_overexplain',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'pg_overexplain',
    ],
  },
}
