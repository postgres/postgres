# Copyright (c) 2022-2024, PostgreSQL Global Development Group

tests += {
  'name': 'brin',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'isolation': {
    'specs': [
      'summarization-and-inprogress-insertion',
    ],
  },
  'tap': {
    'tests': [
      't/01_workitems.pl',
      't/02_wal_consistency.pl',
    ],
  },
}
