# Copyright (c) 2022-2025, PostgreSQL Global Development Group

# libraries that other subsystems might depend upon first, in their respective
# dependency order

subdir('timezone')

subdir('backend')

subdir('bin')

subdir('pl')

subdir('interfaces')

subdir('tools/pg_bsd_indent')


### Generate a Makefile.global that's complete enough for PGXS to work.
#
# This is somewhat ugly, but allows extensions to use a single buildsystem
# across all the supported postgres versions. Once all supported PG versions
# support meson, we can remove all of this.
#
# XXX: Should we make this optional?

# pgxs_cdata is built in makefiles/meson.build, but some of the generated
# files are output into src/
subdir('makefiles')

makefile_global = configure_file(
  input: 'Makefile.global.in',
  output: 'Makefile.global',
  configuration: pgxs_cdata,
  install: true,
  install_dir: dir_pgxs / 'src',
)
configure_files += makefile_global

makefile_port = configure_file(
  input: 'makefiles' / 'Makefile.@0@'.format(portname),
  output: 'Makefile.port',
  copy: true,
  install_dir: dir_pgxs / 'src')
configure_files += makefile_port

install_data(
  'Makefile.shlib', 'nls-global.mk',
  install_dir: dir_pgxs / 'src')

install_data(
  'makefiles/pgxs.mk',
  install_dir: dir_pgxs / 'src' / 'makefiles')
