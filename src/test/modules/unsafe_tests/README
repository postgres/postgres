This directory doesn't actually contain any extension module.

Instead it is a home for regression tests that we don't want to run
during "make installcheck" because they could have side-effects that
seem undesirable for a production installation.

An example is that rolenames.sql tests ALTER USER ALL and so could
have effects on pre-existing roles.
