# Copyright (c) 2022-2026, PostgreSQL Global Development Group

if not get_option('injection_points')
  subdir_done()
endif

tests += {
  'name': 'nbtree',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'nbtree_half_dead_pages',
      'nbtree_incomplete_splits',
    ],
  },
}
