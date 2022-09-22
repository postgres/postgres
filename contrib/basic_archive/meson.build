basic_archive_sources = files(
  'basic_archive.c',
)

basic_archive = shared_module('basic_archive',
  basic_archive_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += basic_archive

tests += {
  'name': 'basic_archive',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'basic_archive',
    ],
    'regress_args': [
      '--temp-config', files('basic_archive.conf'),
    ],
  },
}
