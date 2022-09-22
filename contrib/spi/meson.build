autoinc = shared_module('autoinc',
  ['autoinc.c'],
  kwargs: contrib_mod_args,
)
contrib_targets += autoinc

install_data('autoinc.control', 'autoinc--1.0.sql',
  kwargs: contrib_data_args,
)


insert_username = shared_module('insert_username',
  ['insert_username.c'],
  kwargs: contrib_mod_args,
)
contrib_targets += insert_username

install_data(
  'insert_username.control',
  'insert_username--1.0.sql',
  kwargs: contrib_data_args,
)


moddatetime = shared_module('moddatetime',
  ['moddatetime.c'],
  kwargs: contrib_mod_args,
)
contrib_targets += moddatetime

install_data(
  'moddatetime.control',
  'moddatetime--1.0.sql',
  kwargs: contrib_data_args,
)

# this is needed for the regression tests;
# comment out if you want a quieter refint package for other uses
refint_cflags = ['-DREFINT_VERBOSE']

refint = shared_module('refint',
  ['refint.c'],
  c_args: refint_cflags,
  kwargs: contrib_mod_args,
)
contrib_targets += refint

install_data('refint.control', 'refint--1.0.sql',
  kwargs: contrib_data_args,
)
