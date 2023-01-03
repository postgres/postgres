#
# Copyright 2021 VMware, Inc.
# SPDX-License-Identifier: PostgreSQL
#

# Only Python 3 is supported, but if it's named something different on your
# system you can override it with the PYTHON3 variable.
PYTHON3 := python3

# All dependencies are placed into this directory. The default is .gitignored
# for you, but you can override it if you'd like.
VENV := ./venv

override VBIN   := $(VENV)/bin
override PIP    := $(VBIN)/pip
override PYTEST := $(VBIN)/py.test
override ISORT  := $(VBIN)/isort
override BLACK  := $(VBIN)/black

.PHONY: installcheck indent

installcheck: $(PYTEST)
	$(PYTEST) -v -rs

indent: $(ISORT) $(BLACK)
	$(ISORT) --profile black *.py client/*.py server/*.py
	$(BLACK) *.py client/*.py server/*.py

$(PYTEST) $(ISORT) $(BLACK) &: requirements.txt | $(PIP)
	$(PIP) install --force-reinstall -r $<

$(PIP):
	$(PYTHON3) -m venv $(VENV)

# A convenience recipe to rebuild psycopg2 against the local libpq.
.PHONY: rebuild-psycopg2
rebuild-psycopg2: | $(PIP)
	$(PIP) install --force-reinstall --no-binary :all: $(shell grep psycopg2 requirements.txt)
