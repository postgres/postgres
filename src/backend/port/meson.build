# Copyright (c) 2022-2024, PostgreSQL Global Development Group

backend_sources += files(
  'atomics.c',
)


if cdata.has('USE_UNNAMED_POSIX_SEMAPHORES') or cdata.has('USE_NAMED_POSIX_SEMAPHORES')
  backend_sources += files('posix_sema.c')
endif

if cdata.has('USE_SYSV_SEMAPHORES')
  backend_sources += files('sysv_sema.c')
endif

if cdata.has('USE_WIN32_SEMAPHORES')
  backend_sources += files('win32_sema.c')
endif

if cdata.has('USE_SYSV_SHARED_MEMORY')
  backend_sources += files('sysv_shmem.c')
endif

if cdata.has('USE_WIN32_SHARED_MEMORY')
  backend_sources += files('win32_shmem.c')
endif

if host_system == 'windows'
  subdir('win32')
endif

# autoconf generates the file there, ensure we get a conflict
generated_sources_ac += {'src/backend/port': ['pg_sema.c', 'pg_shmem.c', 'tas.s']}
