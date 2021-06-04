# Copyright (c) 2023, PostgreSQL Global Development Group

subdir('server')

pytest_env = {
  'with_libcurl': oauth_flow_supported ? 'yes' : 'no',

  # Point to the default database; the tests will create their own databases as
  # needed.
  'PGDATABASE': 'postgres',

  # Avoid the need for a Rust compiler on platforms without prebuilt wheels for
  # pyca/cryptography.
  'CRYPTOGRAPHY_DONT_BUILD_RUST': '1',
}

# Some modules (psycopg2) need OpenSSL at compile time; for platforms where we
# might have multiple implementations installed (macOS+brew), try to use the
# same one that libpq is using.
if ssl.found()
  pytest_incdir = ssl.get_variable(pkgconfig: 'includedir', default_value: '')
  if pytest_incdir != ''
    pytest_env += { 'CPPFLAGS': '-I@0@'.format(pytest_incdir) }
  endif

  pytest_libdir = ssl.get_variable(pkgconfig: 'libdir', default_value: '')
  if pytest_libdir != ''
    pytest_env += { 'LDFLAGS': '-L@0@'.format(pytest_libdir) }
  endif
endif

tests += {
  'name': 'python',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'pytest': {
	'requirements': meson.current_source_dir() / 'requirements.txt',
    'tests': [
      './client',
      './server',
      './test_internals.py',
      './test_pq3.py',
    ],
    'env': pytest_env,
    'test_kwargs': {'priority': 50}, # python tests are slow, start early
  },
}
