# Copyright (c) 2022-2026, PostgreSQL Global Development Group

unaccent_sources = files(
  'unaccent.c',
)

if host_system == 'windows'
  unaccent_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'unaccent',
    '--FILEDESC', 'unaccent - text search dictionary that removes accents',])
endif

unaccent = shared_module('unaccent',
  unaccent_sources,
  kwargs: contrib_mod_args,
)
contrib_targets += unaccent

install_data(
  'unaccent--1.0--1.1.sql',
  'unaccent--1.1.sql',
  'unaccent.control',
  kwargs: contrib_data_args,
)

install_data(
  'unaccent.rules',
  install_dir: dir_data / 'tsearch_data'
)

tests += {
  'name': 'unaccent',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'unaccent',
    ],
  },
}


# Download CLDR files on demand.

cldr_baseurl = 'https://raw.githubusercontent.com/unicode-org/cldr/release-@0@/common/transforms/@1@'

if not wget.found() or not cp.found()
  subdir_done()
endif

foreach f : ['Latin-ASCII.xml']
  # XXX could use .replace when we require meson 0.58
  cldr_version_dashed = '-'.join(CLDR_VERSION.split('.'))
  url = cldr_baseurl.format(cldr_version_dashed, f)
  target = custom_target(f,
    output: f,
    command: [wget, wget_flags, url],
    build_by_default: false,
  )
  unicode_data += {f: target}
endforeach

unaccent_update_unicode_targets = \
  custom_target('unaccent.rules',
    input: [unicode_data['UnicodeData.txt'], unicode_data['Latin-ASCII.xml']],
    output: ['unaccent.rules'],
    command: [python, files('generate_unaccent_rules.py'), '--unicode-data-file', '@INPUT0@', '--latin-ascii-file', '@INPUT1@'],
    build_by_default: false,
    capture: true,
  )

update_unicode_unaccent = custom_target('update-unicode',
  output: ['dont-exist'],
  input: unaccent_update_unicode_targets,
  command: [cp, '@INPUT@', '@SOURCE_ROOT@/contrib/unaccent/'],
  build_by_default: false,
  build_always_stale: true,
)

update_unicode_targets += update_unicode_unaccent
