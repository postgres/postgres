# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pg_bsd_indent_sources = files(
  'args.c',
  'err.c',
  'indent.c',
  'io.c',
  'lexi.c',
  'parse.c',
  'pr_comment.c',
)

if host_system == 'windows'
  pg_bsd_indent_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'pg_bsd_indent',
    '--FILEDESC', 'pg_bsd_indent - indent C code nicely'])
endif

pg_bsd_indent = executable('pg_bsd_indent',
  pg_bsd_indent_sources,
  c_args: cflags_no_missing_var_decls,
  dependencies: [frontend_code],
  include_directories: include_directories('.'),
  kwargs: default_bin_args + {
    'install': false,
# possibly at some point do this:
#   'install_dir': dir_pgxs / 'src/tools/pg_bsd_indent',
  },
)
bin_targets += pg_bsd_indent

tests += {
  'name': 'pg_bsd_indent',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_pg_bsd_indent.pl',
    ],
  },
}
