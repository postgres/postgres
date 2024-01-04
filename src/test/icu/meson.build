# Copyright (c) 2022-2024, PostgreSQL Global Development Group

tests += {
  'name': 'icu',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/010_database.pl',
    ],
    'env': {'with_icu': icu.found() ? 'yes' : 'no'},
  },
}
