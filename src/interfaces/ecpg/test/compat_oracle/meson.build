# Copyright (c) 2022-2024, PostgreSQL Global Development Group

pgc_files = [
  'char_array',
]

foreach pgc_file : pgc_files
  exe_input = custom_target('@0@.c'.format(pgc_file),
    input: '@0@.pgc'.format(pgc_file),
    output: '@BASENAME@.c',
    command: ecpg_preproc_test_command_start +
      ['-C', 'ORACLE',] +
      ecpg_preproc_test_command_end,
    install: false,
    build_by_default: false,
  )

  ecpg_test_dependencies += executable(pgc_file,
    exe_input,
    kwargs: ecpg_test_exec_kw,
  )
endforeach
