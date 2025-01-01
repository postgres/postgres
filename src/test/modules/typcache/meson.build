# Copyright (c) 2022-2025, PostgreSQL Global Development Group

if not get_option('injection_points')
  subdir_done()
endif

tests += {
  'name': 'typcache',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'typcache_rel_type_cache',
    ],
  },
}
