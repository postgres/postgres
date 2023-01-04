# Copyright (c) 2022-2023, PostgreSQL Global Development Group

docs = []
alldocs = []
doc_generated = []

xmllint_bin = find_program(get_option('XMLLINT'), native: true, required: false)


version_sgml = configure_file(
  input: 'version.sgml.in',
  output: 'version.sgml',
  configuration: cdata,
)
configure_files += version_sgml

doc_generated += custom_target('features-supported.sgml',
  input: files(
    '../../../src/backend/catalog/sql_feature_packages.txt',
    '../../../src/backend/catalog/sql_features.txt',
  ),
  output: 'features-supported.sgml',
  command: [perl, files('mk_feature_tables.pl'), 'YES', '@INPUT@'],
  build_by_default: false,
  install: false,
  capture: true,
)

doc_generated += custom_target('features-unsupported.sgml',
  input: files(
    '../../../src/backend/catalog/sql_feature_packages.txt',
    '../../../src/backend/catalog/sql_features.txt',
  ),
  output: 'features-unsupported.sgml',
  command: [perl, files('mk_feature_tables.pl'), 'NO', '@INPUT@'],
  build_by_default: false,
  install: false,
  capture: true,
)

doc_generated += custom_target('errcodes-table.sgml',
  input: files(
    '../../../src/backend/utils/errcodes.txt'),
  output: 'errcodes-table.sgml',
  command: [perl, files('generate-errcodes-table.pl'), '@INPUT@'],
  build_by_default: false,
  install: false,
  capture: true,
)

# FIXME: this actually has further inputs, adding depfile support to
# generate-keywords-table.pl is probably the best way to address that
# robustly.
doc_generated += custom_target('keywords-table.sgml',
  input: files(
    '../../../src/include/parser/kwlist.h'),
  output: 'keywords-table.sgml',
  command: [perl, files('generate-keywords-table.pl'), '@CURRENT_SOURCE_DIR@'],
  build_by_default: false,
  install: false,
  capture: true,
)

# For everything else we need at least xmllint
if not xmllint_bin.found()
  subdir_done()
endif

pandoc = find_program('pandoc', native: true, required: false)
xsltproc_bin = find_program(get_option('XSLTPROC'), native: true, required: false)
fop = find_program('fop', native: true, required: false)

xmltools_wrapper = [
  python, files('xmltools_dep_wrapper'),
  '--targetname', '@OUTPUT@', '--depfile', '@DEPFILE@'
]

xmllint = xmltools_wrapper + [
  '--tool', xmllint_bin, '--',
]

# Run validation only once, common to all subsequent targets.  While
# we're at it, also resolve all entities (that is, copy all included
# files into one big file).  This helps tools that don't understand
# vpath builds (such as dbtoepub).
postgres_full_xml = custom_target('postgres-full.xml',
  input: 'postgres.sgml',
  output: 'postgres-full.xml',
  depfile: 'postgres-full.xml.d',
  command: [xmllint, '--noent', '--valid', '--path', '@OUTDIR@', '-o', '@OUTPUT@', '@INPUT@'],
  depends: doc_generated,
  build_by_default: false,
)
docs += postgres_full_xml
alldocs += postgres_full_xml


#
# Full documentation as html, text
#
if xsltproc_bin.found()
  xsltproc_flags = [
    '--stringparam', 'pg.version', pg_version,
    '--param', 'website.stylesheet', '1'
  ]

  xsltproc = xmltools_wrapper + [
    '--tool', xsltproc_bin, '--',
  ]

  html = custom_target('html',
    input: ['stylesheet.xsl', postgres_full_xml],
    output: 'html',
    depfile: 'html.d',
    depends: doc_generated,
    command: [xsltproc, '-o', '@OUTDIR@/', xsltproc_flags, '@INPUT@'],
    build_by_default: false,
  )
  alldocs += html

  # build multi-page html docs as part of docs target
  docs += html

  html_help = custom_target('html_help',
    input: ['stylesheet-hh.xsl', postgres_full_xml],
    output: 'htmlhelp',
    depfile: 'htmlhelp.d',
    depends: doc_generated,
    command: [xsltproc, '--path', '@OUTDIR@', '-o', '@OUTDIR@/', xsltproc_flags, '@INPUT@'],
    build_by_default: false,
  )
  alldocs += html_help


  # single-page HTML
  postgres_html = custom_target('postgres.html',
    input: ['stylesheet-html-nochunk.xsl', postgres_full_xml],
    output: 'postgres.html',
    depfile: 'postgres.html.d',
    depends: doc_generated,
    command: [xsltproc, '--path', '@OUTDIR@', '-o', '@OUTPUT@', xsltproc_flags, '@INPUT@'],
    build_by_default: false,
  )
  alldocs += postgres_html

  # single-page text
  if pandoc.found()
    postgres_txt = custom_target('postgres.txt',
      input: [postgres_html],
      output: 'postgres.txt',
      depends: doc_generated,
      command: [pandoc, '-t', 'plain', '-o', '@OUTPUT@', '@INPUT@'],
      build_by_default: false,
    )
    alldocs += postgres_txt
  endif
endif


#
# INSTALL in html, text
#
if xsltproc_bin.found()
  install_xml = custom_target('INSTALL.xml',
    input: ['standalone-profile.xsl', 'standalone-install.xml'],
    output: 'INSTALL.xml',
    depfile: 'INSTALL.xml.d',
    depends: doc_generated + [postgres_full_xml],
    command: [xsltproc, '--path', '@OUTDIR@', '-o', '@OUTPUT@', xsltproc_flags, '--xinclude', '@INPUT@'],
    build_by_default: false,
  )
  install_html = custom_target('INSTALL.html',
    input: ['stylesheet-text.xsl', install_xml],
    output: 'INSTALL.html',
    depfile: 'INSTALL.html.d',
    command: [xsltproc, '--path', '@OUTDIR@', '-o', '@OUTPUT@', xsltproc_flags, '@INPUT@'],
    build_by_default: false,
  )
  alldocs += install_html

  if pandoc.found()
    # XXX: Makefile does an iconv translit here, but unclear why?
    install = custom_target('INSTALL',
      input: [install_html],
      output: 'INSTALL',
      command: [pandoc, '-t', 'plain', '-o', '@OUTPUT@', '@INPUT@'],
      build_by_default: false,
    )
    alldocs += install
  endif

endif


#
# Man pages
#
if xsltproc_bin.found()
  # FIXME: implement / consider sqlmansectnum logic
  man = custom_target('man',
    input: ['stylesheet-man.xsl', postgres_full_xml],
    output: ['man1', 'man3', 'man7'],
    depfile: 'man.d',
    depends: doc_generated,
    command: [xsltproc, '--path', '@OUTDIR@', '-o', '@OUTDIR@/', xsltproc_flags, '@INPUT@'],
    build_by_default: false,
  )

  # build by default as part of docs target
  docs += man
endif


#
# Full documentation as PDF
#
if fop.found() and xsltproc_bin.found()
  xsltproc_fo_flags = xsltproc_flags + ['--stringparam', 'img.src.path', meson.current_source_dir() + '/']

  foreach format, detail: {'A4': 'A4', 'US': 'USletter'}
    postgres_x_fo_f = 'postgres-@0@.fo'.format(format)
    postgres_x_fo_dep = 'postgres-@0@.fo.d'.format(format)
    postgres_x_pdf_f = 'postgres-@0@.pdf'.format(format)

    postgres_x_fo = custom_target(postgres_x_fo_f,
      input: ['stylesheet-fo.xsl', postgres_full_xml],
      output: postgres_x_fo_f,
      depends: doc_generated,
      depfile: postgres_x_fo_dep,
      command: [xsltproc, '--path', '@OUTDIR@/', xsltproc_fo_flags,
                '--stringparam', 'paper.type', detail,
                '-o', '@OUTPUT@', '@INPUT@'],
      build_by_default: false,
    )

    postgres_x_pdf = custom_target(postgres_x_pdf_f,
      input: [postgres_x_fo],
      output: [postgres_x_pdf_f],
      command: [fop, '-fo', '@INPUT@', '-pdf', '@OUTPUT@'],
      build_by_default: false,
    )
    alldocs += postgres_x_pdf
  endforeach
endif


#
# epub
#

# This was previously implemented using dbtoepub - but that doesn't seem to
# support running in build != source directory (i.e. VPATH builds already
# weren't supported).
if pandoc.found() and xsltproc_bin.found()
  postgres_epub = custom_target('postgres.epub',
    input: postgres_full_xml,
    output: 'postgres.epub',
    command: [pandoc, '-f', 'docbook', '-t', 'epub', '-o', '@OUTPUT@', '--resource-path=@CURRENT_SOURCE_DIR@',
              '@INPUT@'],
    build_by_default: false,
  )
  alldocs += postgres_epub
endif

if docs.length() == 0
  run_target('docs', command: [missing, 'xsltproc'])
else
  alias_target('docs', docs)
endif

if alldocs.length() == 0
  run_target('alldocs', command: [missing, 'xsltproc'])
else
  alias_target('alldocs', alldocs)
endif
