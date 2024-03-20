# Copyright (c) 2022-2024, PostgreSQL Global Development Group

lwlocknames_h = custom_target('lwlocknames_h',
  input: files(
    '../../include/storage/lwlocklist.h',
    '../../backend/utils/activity/wait_event_names.txt'),
  output: ['lwlocknames.h'],
  command: [
    perl, files('../../backend/storage/lmgr/generate-lwlocknames.pl'),
    '-o', '@OUTDIR@',
    '@INPUT@'
  ],
  build_by_default: true,
  install: true,
  install_dir: dir_include_server / 'storage',
)

generated_backend_headers += lwlocknames_h

# autoconf generates the file there, ensure we get a conflict
generated_sources_ac += {'src/backend/storage/lmgr': ['lwlocknames.h']}
