# Copyright (c) 2022-2025, PostgreSQL Global Development Group

libpq_pipeline_sources = files(
  'libpq_pipeline.c',
)

if host_system == 'windows'
  libpq_pipeline_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'libpq_pipeline',
    '--FILEDESC', 'libpq_pipeline - test program for pipeline execution',])
endif

libpq_pipeline = executable('libpq_pipeline',
  libpq_pipeline_sources,
  dependencies: [frontend_code, libpq],
  kwargs: default_bin_args + {
    'install': false,
  },
)
testprep_targets += libpq_pipeline

tests += {
  'name': 'libpq_pipeline',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'tap': {
    'tests': [
      't/001_libpq_pipeline.pl',
    ],
  },
}
