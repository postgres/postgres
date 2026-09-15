# doc/src/sgml/images/meson.build
#
# see README in this directory about image handling

if not xsltproc_bin.found() or not dot.found() or not ditaa.found()
  subdir_done()
endif

image_targets = []

fixup_svg_xsl = files('fixup-svg.xsl')

all_files = [
  'genetic-algorithm.gv',
  'gin.gv',
  'pagelayout.txt',
  'temporal-entities.txt',
  'temporal-references.txt',
]

foreach file : all_files

  str_split = file.split('.')
  actual_file_name = str_split[0]
  extension = str_split[1]
  cur_file = files(file)
  tmp_name = '@0@.svg.tmp'.format(file)
  output_name = '@0@.svg'.format(actual_file_name)

  command = []
  if extension == 'gv'
    command = [dot, '-T', 'svg', '-o', '@OUTPUT@', '@INPUT@']
  elif extension == 'txt'
    command = [ditaa, '-E', '-S', '--svg', '@INPUT@', '@OUTPUT@']
  else
    error('Unknown extension: ".@0@" while generating images'.format(extension))
  endif

  svg_tmp = custom_target(tmp_name,
    input: cur_file,
    output: tmp_name,
    command: command,
  )

  current_svg = custom_target(output_name,
    input: svg_tmp,
    output: output_name,
    command: [xsltproc_bin,
      '--nonet',
      # Use --novalid to avoid loading SVG DTD if a file specifies it, since
      # it might not be available locally, and we don't need it.
      '--novalid',
      '-o', '@OUTPUT@',
      fixup_svg_xsl,
      '@INPUT@']
  )

  image_targets += current_svg
endforeach

alias_target('images', image_targets)
