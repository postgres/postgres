# Copyright (c) 2022-2023, PostgreSQL Global Development Group

pgc_files = [
  'array',
  'binary',
  'bytea',
  'code100',
  'copystdout',
  'createtableas',
  'declare',
  'define',
  'desc',
  'describe',
  'dynalloc',
  'dynalloc2',
  'dyntest',
  'execute',
  'fetch',
  'func',
  'indicators',
  'insupd',
  'oldexec',
  'parser',
  'prepareas',
  'quote',
  'show',
  'sqlda',
  'sqljson',
  'twophase',
]

pgc_extra_flags = {
  'oldexec': ['-r', 'questionmarks'],
}

foreach pgc_file : pgc_files
  exe_input = custom_target('@0@.c'.format(pgc_file),
    input: '@0@.pgc'.format(pgc_file),
    command: ecpg_preproc_test_command_start +
      pgc_extra_flags.get(pgc_file, []) +
      ecpg_preproc_test_command_end,
    kwargs: ecpg_preproc_kw,
  )

  ecpg_test_dependencies += executable(pgc_file,
    exe_input,
    kwargs: ecpg_test_exec_kw,
  )
endforeach
