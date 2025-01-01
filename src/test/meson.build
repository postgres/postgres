# Copyright (c) 2022-2025, PostgreSQL Global Development Group

subdir('regress')
subdir('isolation')

subdir('authentication')
subdir('postmaster')
subdir('recovery')
subdir('subscription')
subdir('modules')

if ssl.found()
  subdir('ssl')
endif

if ldap.found()
  subdir('ldap')
endif

if gssapi.found()
  subdir('kerberos')
endif

if icu.found()
  subdir('icu')
endif

subdir('perl')
