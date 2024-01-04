# Copyright (c) 2022-2024, PostgreSQL Global Development Group

test_shm_mq_sources = files(
  'setup.c',
  'test.c',
  'worker.c',
)

if host_system == 'windows'
  test_shm_mq_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_shm_mq',
    '--FILEDESC', 'test_shm_mq - example use of shared memory message queue',])
endif

test_shm_mq = shared_module('test_shm_mq',
  test_shm_mq_sources,
  kwargs: pg_test_mod_args,
)
test_install_libs += test_shm_mq

test_install_data += files(
  'test_shm_mq.control',
  'test_shm_mq--1.0.sql',
)

tests += {
  'name': 'test_shm_mq',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'test_shm_mq',
    ],
  },
}
