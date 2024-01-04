#-------------------------------------------------------------------------
#
# Makefile for sslfiles
#
#   The SSL test files are completely disjoint from the rest of the build; they
#   don't rely on other targets or on Makefile.global. Since these recipes rely
#   on some default Make behavior that's disabled in the main build tree, such
#   as intermediate cleanup, they've been moved into their own separate file.
#   The main Makefile in this directory defers to this helper file when
#   building the sslfiles-related targets.
#
# Portions Copyright (c) 1996-2024, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/ssl/sslfiles.mk
#
#-------------------------------------------------------------------------

#
# To add a new server or client certificate, add a new <name>.config file in
# the conf/ directory, then add <name> to either SERVERS or CLIENTS below. A
# key/certificate pair will be generated for you, signed by the appropriate CA.
#
SERVERS := server-cn-and-alt-names \
	server-cn-and-ip-alt-names \
	server-cn-only \
	server-ip-alt-names \
	server-ip-cn-only \
	server-ip-cn-and-alt-names \
	server-ip-cn-and-dns-alt-names \
	server-ip-in-dnsname \
	server-single-alt-name \
	server-multiple-alt-names \
	server-no-names \
	server-revoked
CLIENTS := client client-dn client-revoked client_ext client-long \
	client-revoked-utf8

#
# To add a new non-standard certificate, add it to SPECIAL_CERTS and then add
# a recipe for creating it to the "Special-case certificates" section below.
#
SPECIAL_CERTS := ssl/server-rsapss.crt

# Likewise for non-standard keys
SPECIAL_KEYS := ssl/server-password.key \
	ssl/client-der.key \
	ssl/client-encrypted-pem.key \
	ssl/client-encrypted-der.key \
	ssl/server-rsapss.key

#
# These files are just concatenations of other files. You can add new ones to
# COMBINATIONS here, then declare the constituent files as dependencies in the
# "Combined files" section below.
#
COMBINATIONS := \
	ssl/both-cas-1.crt \
	ssl/both-cas-2.crt \
	ssl/root+server_ca.crt \
	ssl/root+server.crl \
	ssl/root+client_ca.crt \
	ssl/root+client.crl \
	ssl/client+client_ca.crt \
	ssl/server-cn-only+server_ca.crt

CERTIFICATES := root_ca server_ca client_ca $(SERVERS) $(CLIENTS)
STANDARD_CERTS := $(CERTIFICATES:%=ssl/%.crt)
STANDARD_KEYS := $(CERTIFICATES:%=ssl/%.key)
CRLS := ssl/root.crl \
	ssl/client.crl \
	ssl/server.crl

SSLFILES := \
	$(STANDARD_CERTS) \
	$(STANDARD_KEYS) \
	$(SPECIAL_CERTS) \
	$(SPECIAL_KEYS) \
	$(COMBINATIONS) \
	$(CRLS)
SSLDIRS := ssl/client-crldir \
	ssl/server-crldir \
	ssl/root+client-crldir \
	ssl/root+server-crldir

# This target re-generates all the key and certificate files. Usually we just
# use the ones that are committed to the tree without rebuilding them.
#
.PHONY: sslfiles
sslfiles: $(SSLFILES) $(SSLDIRS)

#
# Special-case certificates
#

# Root CA is self-signed.
ssl/root_ca.crt: ssl/root_ca.key conf/root_ca.config
	$(OPENSSL) req -new -x509 -config conf/root_ca.config -days 10000 -key $< -out $@

# Certificate using RSA-PSS algorithm. Also self-signed.
ssl/server-rsapss.crt: ssl/server-rsapss.key conf/server-rsapss.config
	$(OPENSSL) req -new -x509 -config conf/server-rsapss.config -key $< -out $@

#
# Special-case keys
#
# All targets here are contained in $(SPECIAL_KEYS).
#

# Password-protected version of server-cn-only.key
ssl/server-password.key: ssl/server-cn-only.key
	$(OPENSSL) pkey -aes256 -in $< -out $@ -passout 'pass:secret1'

# Key that uses the RSA-PSS algorithm
ssl/server-rsapss.key:
	$(OPENSSL) genpkey -algorithm rsa-pss -out $@

# DER-encoded version of client.key
ssl/client-der.key: ssl/client.key
	$(OPENSSL) rsa -in $< -outform DER -out $@

# Convert client.key to encrypted PEM (X.509 text) and DER (X.509 ASN.1)
# formats to test libpq's support for the sslpassword= option.
ssl/client-encrypted-pem.key: ssl/client.key
	$(OPENSSL) pkey -in $< -outform PEM -aes128 -passout 'pass:dUmmyP^#+' -out $@
# TODO Explicitly choosing -aes128 generates a key unusable to PostgreSQL with
# OpenSSL 3.0.0, so fall back on the default for now.
ssl/client-encrypted-der.key: ssl/client.key
	$(OPENSSL) rsa -in $< -outform DER -passout 'pass:dUmmyP^#+' -out $@

#
# Combined files
#
# All targets in $(COMBINATIONS) share a single recipe; just declare the
# necessary dependencies and they'll be smashed together.
#

# Root certificate file that contains both CA certificates, for testing
# that multiple certificates can be used.
ssl/both-cas-1.crt: ssl/root_ca.crt ssl/client_ca.crt ssl/server_ca.crt

# The same, but the certs are in different order
ssl/both-cas-2.crt: ssl/root_ca.crt ssl/server_ca.crt ssl/client_ca.crt

# A root certificate file for the client, to validate server certs.
ssl/root+server_ca.crt: ssl/root_ca.crt ssl/server_ca.crt

# and for the server, to validate client certs
ssl/root+client_ca.crt: ssl/root_ca.crt ssl/client_ca.crt

# and for the client, to present to the server
ssl/client+client_ca.crt: ssl/client.crt ssl/client_ca.crt

# for the server, to present to a client that only knows the root
ssl/server-cn-only+server_ca.crt: ssl/server-cn-only.crt ssl/server_ca.crt

# If a CRL is used, OpenSSL requires a CRL file for *all* the CAs in the
# chain, even if some of them are empty.
ssl/root+server.crl: ssl/root.crl ssl/server.crl
ssl/root+client.crl: ssl/root.crl ssl/client.crl

$(COMBINATIONS):
	cat $^ > $@

#
# Standard keys
#

$(STANDARD_KEYS):
	$(OPENSSL) genrsa -out $@ 2048
	chmod 0600 $@

#
# Standard certificates
#

CA_CERTS     := ssl/server_ca.crt ssl/client_ca.crt
SERVER_CERTS := $(SERVERS:%=ssl/%.crt)
CLIENT_CERTS := $(CLIENTS:%=ssl/%.crt)

# See the "CA State" section below.
root_ca_state_files := ssl/root_ca-certindex ssl/root_ca-certindex.attr ssl/root_ca.srl
server_ca_state_files := ssl/server_ca-certindex ssl/server_ca-certindex.attr ssl/server_ca.srl
client_ca_state_files := ssl/client_ca-certindex ssl/client_ca-certindex.attr ssl/client_ca.srl

# These are the workhorse recipes. `openssl ca` can't be safely run from
# parallel processes, so we must mark the entire Makefile .NOTPARALLEL.
.NOTPARALLEL:
$(CA_CERTS): ssl/%.crt: ssl/%.csr conf/%.config conf/cas.config ssl/root_ca.crt | ssl/new_certs_dir $(root_ca_state_files)
	$(OPENSSL) ca -batch -config conf/cas.config -name root_ca   -notext -in $< -out $@

$(SERVER_CERTS): ssl/%.crt: ssl/%.csr conf/%.config conf/cas.config ssl/server_ca.crt | ssl/new_certs_dir $(server_ca_state_files)
	$(OPENSSL) ca -batch -config conf/cas.config -name server_ca -notext -in $< -out $@

$(CLIENT_CERTS): ssl/%.crt: ssl/%.csr conf/%.config conf/cas.config ssl/client_ca.crt | ssl/new_certs_dir $(client_ca_state_files)
	$(OPENSSL) ca -batch -config conf/cas.config -name client_ca -notext -in $< -out $@

# The CSRs don't need to persist after a build.
.INTERMEDIATE: $(CERTIFICATES:%=ssl/%.csr)
ssl/%.csr: ssl/%.key conf/%.config
	$(OPENSSL) req -new -utf8 -key $< -out $@ -config conf/$*.config

#
# CA State
#
# All of these are intended to be order-only dependencies; additionally, the
# pattern recipes are marked as explicit intermediates. The goal is for Make to
# create the state files once for each CA, allow them to accumulate whatever
# state is needed, and then automatically remove them at the end of the run.
#

.INTERMEDIATE: $(root_ca_state_files) $(server_ca_state_files) $(client_ca_state_files)

# OpenSSL requires a directory to put all generated certificates in. We don't
# use this for anything, but we need a location.
ssl/new_certs_dir:
	mkdir $@

ssl/%-certindex:
	touch $@

ssl/%-certindex.attr:
	echo "unique_subject=no" > $@

# The first serial number for each CA is based on the current timestamp, to
# avoid collisions across Make runs.
ssl/%.srl:
	date +%Y%m%d%H%M%S00 > $@

#
# CRLs
#

ssl/root.crl: ssl/root_ca.crt | $(root_ca_state_files)
	$(OPENSSL) ca -config conf/cas.config -name root_ca   -gencrl -out $@

ssl/server.crl: ssl/server-revoked.crt ssl/server_ca.crt | $(server_ca_state_files)
	$(OPENSSL) ca -config conf/cas.config -name server_ca -revoke $<
	$(OPENSSL) ca -config conf/cas.config -name server_ca -gencrl -out $@

ssl/client.crl: ssl/client-revoked.crt ssl/client-revoked-utf8.crt ssl/client_ca.crt | $(client_ca_state_files)
	$(OPENSSL) ca -config conf/cas.config -name client_ca -revoke ssl/client-revoked.crt
	$(OPENSSL) ca -config conf/cas.config -name client_ca -revoke ssl/client-revoked-utf8.crt
	$(OPENSSL) ca -config conf/cas.config -name client_ca -gencrl -out $@

#
# CRL hash directories
#

ssl/root+server-crldir: ssl/server.crl ssl/root.crl
ssl/root+client-crldir: ssl/client.crl ssl/root.crl
ssl/server-crldir: ssl/server.crl
ssl/client-crldir: ssl/client.crl

crlhashfile = $(shell $(OPENSSL) crl -hash -noout -in $(1)).r0

ssl/%-crldir:
	mkdir -p $@
	rm -f $@/*.r0
	$(foreach crl,$^,cp $(crl) $@/$(call crlhashfile,$(crl)) &&) true
	touch $@

.PHONY: sslfiles-clean
sslfiles-clean:
	rm -f $(SSLFILES) ssl/*.old ssl/*.csr ssl/*.srl ssl/*-certindex*
	rm -rf $(SSLDIRS) ssl/new_certs_dir

# The difference between the below clean targets and sslfiles-clean is that the
# clean targets will be run during a "standard" recursive clean run from the
# main build tree. The sslfiles-clean target must be run explicitly from this
# directory.
.PHONY: clean distclean
clean distclean:
	rm -rf ssl/*.old ssl/new_certs_dir ssl/client*_tmp.key
