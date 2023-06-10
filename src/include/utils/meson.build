# Copyright (c) 2022-2023, PostgreSQL Global Development Group

errcodes = custom_target('errcodes',
  input: files('../../backend/utils/errcodes.txt'),
  output: ['errcodes.h'],
  command: [
    perl, files('../../backend/utils/generate-errcodes.pl'),
    '--outfile', '@OUTPUT@',
    '@INPUT@',
  ],
  install: true,
  install_dir: dir_include_server / 'utils',
)
generated_headers += errcodes

if dtrace.found()
  probes_tmp = custom_target('probes.h.tmp',
    input: files('../../backend/utils/probes.d'),
    output: 'probes.h.tmp',
    command: [dtrace, '-C', '-h', '-s', '@INPUT@', '-o', '@OUTPUT@'],
  )
  probes = custom_target('probes.h',
    input: probes_tmp,
    output: 'probes.h',
    capture: true,
    command: [sed, '-f', files('../../backend/utils/postprocess_dtrace.sed'), '@INPUT@'],
    install: true,
    install_dir: dir_include_server / 'utils',
  )
else
  probes = custom_target('probes.h',
    input: files('../../backend/utils/probes.d'),
    output: 'probes.h',
    capture: true,
    command: [sed, '-f', files('../../backend/utils/Gen_dummy_probes.sed'), '@INPUT@'],
    install: true,
    install_dir: dir_include_server / 'utils',
  )
endif

generated_backend_headers += probes

fmgrtab_output = ['fmgroids.h', 'fmgrprotos.h', 'fmgrtab.c']
fmgrtab_target = custom_target('fmgrtab',
  input: '../catalog/pg_proc.dat',
  output : fmgrtab_output,
  depend_files: catalog_pm,
  command: [perl, '-I', '@SOURCE_ROOT@/src/backend/catalog/', files('../../backend/utils/Gen_fmgrtab.pl'), '--include-path=@SOURCE_ROOT@/src/include', '--output=@OUTDIR@', '@INPUT@'],
  install: true,
  install_dir: [dir_include_server / 'utils', dir_include_server / 'utils', false],
)

generated_backend_headers += fmgrtab_target[0]
generated_backend_headers += fmgrtab_target[1]

# autoconf generates the file there, ensure we get a conflict
generated_sources_ac += {
  'src/backend/utils': fmgrtab_output + ['errcodes.h', 'probes.h', 'fmgr-stamp'],
  'src/include/utils': ['header-stamp'],
}
