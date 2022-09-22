citext_sources = files(
  'citext.c',
)

citext = shared_module('citext',
  citext_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += citext

install_data(
  'citext.control',
  'citext--1.0--1.1.sql',
  'citext--1.1--1.2.sql',
  'citext--1.2--1.3.sql',
  'citext--1.3--1.4.sql',
  'citext--1.4.sql',
  'citext--1.4--1.5.sql',
  'citext--1.5--1.6.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'citext',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'create_index_acl',
      'citext',
      'citext_utf8',
    ],
  },
}
