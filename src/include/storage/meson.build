# Copyright (c) 2022-2023, PostgreSQL Global Development Group

lwlocknames = custom_target('lwlocknames',
  input: files('../../backend/storage/lmgr/lwlocknames.txt'),
  output: ['lwlocknames.h', 'lwlocknames.c'],
  command: [
    perl, files('../../backend/storage/lmgr/generate-lwlocknames.pl'),
    '-o', '@OUTDIR@',
    '@INPUT@'
  ],
  build_by_default: true,
  install: true,
  install_dir: [dir_include_server / 'storage', false],
)

lwlocknames_h = lwlocknames[0]

generated_backend_headers += lwlocknames_h

# autoconf generates the file there, ensure we get a conflict
generated_sources_ac += {'src/backend/storage/lmgr': ['lwlocknames.c', 'lwlocknames.h']}
