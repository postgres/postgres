# Copyright (c) 2022-2025, PostgreSQL Global Development Group

delay_execution_sources = files(
  'delay_execution.c',
)

if host_system == 'windows'
  delay_execution_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'delay_execution',
    '--FILEDESC', 'delay_execution - allow delay between parsing and execution',])
endif

delay_execution = shared_module('delay_execution',
  delay_execution_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += delay_execution

tests += {
  'name': 'delay_execution',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'isolation': {
    'specs': [
      'partition-addition',
      'partition-removal-1',
    ],
  },
}
