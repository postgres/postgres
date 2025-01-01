# Copyright (c) 2022-2025, PostgreSQL Global Development Group

pgc_files = [
  'charfuncs',
  'dec_test',
  'describe',
  'intoasc',
  'rfmtdate',
  'rfmtlong',
  'rnull',
  'sqlda',
  'test_informix',
  'test_informix2',
]

pgc_extra_flags = {
  'rnull': ['-r', 'no_indicator',],
}

foreach pgc_file : pgc_files
  exe_input = custom_target('@0@.c'.format(pgc_file),
    input: '@0@.pgc'.format(pgc_file),
    command: ecpg_preproc_test_command_start +
      ['-C', 'INFORMIX',] +
      pgc_extra_flags.get(pgc_file, []) +
      ecpg_preproc_test_command_end,
    kwargs: ecpg_preproc_kw,
  )

  ecpg_test_dependencies += executable(pgc_file,
    exe_input,
    kwargs: ecpg_test_exec_kw,
  )
endforeach
