ecpg_sources = files(
  '../ecpglib/typename.c',
  'c_keywords.c',
  'descriptor.c',
  'ecpg.c',
  'ecpg_keywords.c',
  'keywords.c',
  'output.c',
  'parser.c',
  'type.c',
  'variable.c',
)

pgc = custom_target('pgc.c',
  input: 'pgc.l',
  output: 'pgc.c',
  command: flex_cmd,
)
generated_sources += pgc
ecpg_sources += pgc

ecpg_files = [
  'ecpg.addons',
  'ecpg.header',
  'ecpg.tokens',
  'ecpg.trailer',
  'ecpg.type',
]

preproc_y = custom_target('preproc.y',
  input: [
    '../../../backend/parser/gram.y',
    ecpg_files,
  ],
  output: ['preproc.y'],
  command: [
    perl, files('parse.pl'),
    '--srcdir', '@CURRENT_SOURCE_DIR@',
    '--parser', '@INPUT0@',
    '--output', '@OUTPUT0@',
  ],
)
generated_sources += preproc_y

check_rules = custom_target('preproc.y.check_rules',
  input: [
    '../../../backend/parser/gram.y',
    ecpg_files,
  ],
  output: 'preproc.y.check_rules',
  command: [
    perl, files('check_rules.pl'),
    '--srcdir', '@CURRENT_SOURCE_DIR@',
    '--parser', '@INPUT0@',
    '--stamp', '@OUTPUT0@',
  ],
)

preproc = custom_target('preproc.c',
  input: preproc_y,
  kwargs: bison_kw,
)
generated_sources += preproc.to_list()
ecpg_sources += preproc

c_kwlist = custom_target('c_kwlist_d.h',
  input: ['c_kwlist.h'],
  output: ['c_kwlist_d.h'],
  depends: check_rules,
  command: [
    perl,
    '-I', '@SOURCE_ROOT@/src/tools',
    '@SOURCE_ROOT@/src/tools/gen_keywordlist.pl',
    '--output', '@OUTDIR@',
    '--varname', 'ScanCKeywords',
    '--no-case-fold', '@INPUT0@',
  ],
)
generated_sources += c_kwlist
ecpg_sources += c_kwlist

ecpg_kwlist = custom_target('ecpg_kwlist_d.h',
  input: ['ecpg_kwlist.h'],
  output: ['ecpg_kwlist_d.h'],
  command: [
    perl, '-I',
    '@SOURCE_ROOT@/src/tools',
    '@SOURCE_ROOT@/src/tools/gen_keywordlist.pl',
    '--output', '@OUTDIR@',
    '--varname', 'ScanECPGKeywords', '@INPUT0@',
  ]
)
generated_sources += ecpg_kwlist
ecpg_sources += ecpg_kwlist

if host_system == 'windows'
  ecpg_sources += rc_bin_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'ecpg',
    '--FILEDESC', 'ecpg - embedded SQL precompiler for C',])
endif

ecpg_exe = executable('ecpg',
  ecpg_sources,
  include_directories: ['.', ecpg_inc, postgres_inc, libpq_inc],
  dependencies: [frontend_code],
  kwargs: default_bin_args,
)
ecpg_targets += ecpg_exe

subdir('po', if_found: libintl)
