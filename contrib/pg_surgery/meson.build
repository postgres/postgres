pg_surgery = shared_module('pg_surgery',
  files(
    'heap_surgery.c',
  ),
  kwargs: contrib_mod_args,
)
contrib_targets += pg_surgery

install_data(
  'pg_surgery--1.0.sql',
  'pg_surgery.control',
  kwargs: contrib_data_args,
)


tests += {
  'name': 'pg_surgery',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'heap_surgery',
    ],
  },
}
