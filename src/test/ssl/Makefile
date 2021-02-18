#-------------------------------------------------------------------------
#
# Makefile for src/test/ssl
#
# Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
# Portions Copyright (c) 1994, Regents of the University of California
#
# src/test/ssl/Makefile
#
#-------------------------------------------------------------------------

subdir = src/test/ssl
top_builddir = ../../..
include $(top_builddir)/src/Makefile.global

export with_ssl

CERTIFICATES := server_ca server-cn-and-alt-names \
	server-cn-only server-single-alt-name server-multiple-alt-names \
	server-no-names server-revoked server-ss \
	client_ca client client-revoked \
	root_ca

SSLFILES := $(CERTIFICATES:%=ssl/%.key) $(CERTIFICATES:%=ssl/%.crt) \
	ssl/server-password.key \
	ssl/client.crl ssl/server.crl ssl/root.crl \
	ssl/both-cas-1.crt ssl/both-cas-2.crt \
	ssl/root+server_ca.crt ssl/root+server.crl \
	ssl/root+client_ca.crt ssl/root+client.crl \
	ssl/client+client_ca.crt ssl/client-der.key \
	ssl/client-encrypted-pem.key ssl/client-encrypted-der.key

SSLDIRS := ssl/client-crldir ssl/server-crldir \
	ssl/root+client-crldir ssl/root+server-crldir

# This target re-generates all the key and certificate files. Usually we just
# use the ones that are committed to the tree without rebuilding them.
#
# This target will fail unless preceded by sslfiles-clean.
#
sslfiles: $(SSLFILES) $(SSLDIRS)

# OpenSSL requires a directory to put all generated certificates in. We don't
# use this for anything, but we need a location.
ssl/new_certs_dir:
	mkdir ssl/new_certs_dir

# Rule for creating private/public key pairs.
ssl/%.key:
	openssl genrsa -out $@ 2048
	chmod 0600 $@

# Root CA certificate
ssl/root_ca.crt: ssl/root_ca.key cas.config
	touch ssl/root_ca-certindex
	openssl req -new -out ssl/root_ca.crt -x509 -config cas.config -config root_ca.config -key ssl/root_ca.key -days 10000 -extensions v3_ca
	echo "01" > ssl/root_ca.srl

# Client and server CAs
ssl/%_ca.crt: ssl/%_ca.key %_ca.config ssl/root_ca.crt ssl/new_certs_dir
	touch ssl/$*_ca-certindex
	echo "unique_subject=no" > ssl/$*_ca-certindex.attr
	openssl req -new -out ssl/temp_ca.crt -config cas.config -config $*_ca.config -key ssl/$*_ca.key
# Sign the certificate with the root CA
	openssl ca -name root_ca -batch -config cas.config -in ssl/temp_ca.crt -out ssl/temp_ca_signed.crt -extensions v3_ca
	openssl x509 -in ssl/temp_ca_signed.crt -out ssl/$*_ca.crt # to keep just the PEM cert
	rm ssl/temp_ca.crt ssl/temp_ca_signed.crt
	echo "01" > ssl/$*_ca.srl

# Server certificates, signed by server CA:
ssl/server-%.crt: ssl/server-%.key ssl/server_ca.crt server-%.config
	openssl req -new -key ssl/server-$*.key -out ssl/server-$*.csr -config server-$*.config
	openssl ca -name server_ca -batch -config cas.config -in ssl/server-$*.csr -out ssl/temp.crt  -extensions v3_req -extfile server-$*.config
	openssl x509 -in ssl/temp.crt -out ssl/server-$*.crt # to keep just the PEM cert
	rm ssl/server-$*.csr

# Self-signed version of server-cn-only.crt
ssl/server-ss.crt: ssl/server-cn-only.key ssl/server-cn-only.crt server-cn-only.config
	openssl req -new -key ssl/server-cn-only.key -out ssl/server-ss.csr -config server-cn-only.config
	openssl x509 -req -days 10000 -in ssl/server-ss.csr -signkey ssl/server-cn-only.key -out ssl/server-ss.crt  -extensions v3_req -extfile server-cn-only.config
	rm ssl/server-ss.csr

# Password-protected version of server-cn-only.key
ssl/server-password.key: ssl/server-cn-only.key
	openssl rsa -aes256 -in $< -out $@ -passout 'pass:secret1'

# Client certificate, signed by the client CA:
ssl/client.crt: ssl/client.key ssl/client_ca.crt
	openssl req -new -key ssl/client.key -out ssl/client.csr -config client.config
	openssl ca -name client_ca -batch -out ssl/temp.crt -config cas.config -infiles ssl/client.csr
	openssl x509 -in ssl/temp.crt -out ssl/client.crt # to keep just the PEM cert
	rm ssl/client.csr ssl/temp.crt

# Another client certificate, signed by the client CA. This one is revoked.
ssl/client-revoked.crt: ssl/client-revoked.key ssl/client_ca.crt client.config
	openssl req -new -key ssl/client-revoked.key -out ssl/client-revoked.csr -config client.config
	openssl ca -name client_ca -batch -out ssl/temp.crt -config cas.config -infiles ssl/client-revoked.csr
	openssl x509 -in ssl/temp.crt -out ssl/client-revoked.crt # to keep just the PEM cert
	rm ssl/client-revoked.csr ssl/temp.crt

# Convert the key to DER, to test our behaviour there too
ssl/client-der.key: ssl/client.key
	openssl rsa -in ssl/client.key -outform DER -out ssl/client-der.key

# Convert the existing key to encrypted PEM (X.509 text) and DER (X.509 ASN.1) formats
# to test libpq's support for the sslpassword= option.
ssl/client-encrypted-pem.key: ssl/client.key
	openssl rsa -in ssl/client.key -outform PEM -aes128 -passout 'pass:dUmmyP^#+' -out ssl/client-encrypted-pem.key

ssl/client-encrypted-der.key: ssl/client.key
	openssl rsa -in ssl/client.key -outform DER -aes128 -passout 'pass:dUmmyP^#+' -out ssl/client-encrypted-der.key

# Root certificate files that contains both CA certificates, for testing
# that multiple certificates can be used.
ssl/both-cas-1.crt: ssl/root_ca.crt ssl/client_ca.crt ssl/server_ca.crt
	cat $^ > $@

# The same, but the certs are in different order
ssl/both-cas-2.crt: ssl/root_ca.crt ssl/server_ca.crt ssl/client_ca.crt
	cat $^ > $@

# A root certificate file for the client, to validate server certs.
ssl/root+server_ca.crt: ssl/root_ca.crt ssl/server_ca.crt
	cat $^ > $@

# and for the server, to validate client certs
ssl/root+client_ca.crt: ssl/root_ca.crt ssl/client_ca.crt
	cat $^ > $@

ssl/client+client_ca.crt: ssl/client.crt ssl/client_ca.crt
	cat $^ > $@

#### CRLs

ssl/client.crl: ssl/client-revoked.crt
	openssl ca -config cas.config -name client_ca -revoke ssl/client-revoked.crt
	openssl ca -config cas.config -name client_ca -gencrl -out ssl/client.crl

ssl/server.crl: ssl/server-revoked.crt
	openssl ca -config cas.config -name server_ca -revoke ssl/server-revoked.crt
	openssl ca -config cas.config -name server_ca -gencrl -out ssl/server.crl

ssl/root.crl: ssl/root_ca.crt
	openssl ca -config cas.config -name root_ca -gencrl -out ssl/root.crl

# If a CRL is used, OpenSSL requires a CRL file for *all* the CAs in the
# chain, even if some of them are empty.
ssl/root+server.crl: ssl/root.crl ssl/server.crl
	cat $^ > $@
ssl/root+client.crl: ssl/root.crl ssl/client.crl
	cat $^ > $@

ssl/root+server-crldir: ssl/server.crl ssl/root.crl
	mkdir ssl/root+server-crldir
	cp ssl/server.crl ssl/root+server-crldir/`openssl crl -hash -noout -in ssl/server.crl`.r0
	cp ssl/root.crl ssl/root+server-crldir/`openssl crl -hash -noout -in ssl/root.crl`.r0

ssl/root+client-crldir: ssl/client.crl ssl/root.crl
	mkdir ssl/root+client-crldir
	cp ssl/client.crl ssl/root+client-crldir/`openssl crl -hash -noout -in ssl/client.crl`.r0
	cp ssl/root.crl ssl/root+client-crldir/`openssl crl -hash -noout -in ssl/root.crl`.r0

ssl/server-crldir: ssl/server.crl
	mkdir ssl/server-crldir
	cp ssl/server.crl ssl/server-crldir/`openssl crl -hash -noout -in ssl/server.crl`.r0

ssl/client-crldir: ssl/client.crl
	mkdir ssl/client-crldir
	cp ssl/client.crl ssl/client-crldir/`openssl crl -hash -noout -in ssl/client.crl`.r0

.PHONY: sslfiles-clean
sslfiles-clean:
	rm -f $(SSLFILES) ssl/client_ca.srl ssl/server_ca.srl ssl/client_ca-certindex* ssl/server_ca-certindex* ssl/root_ca-certindex* ssl/root_ca.srl ssl/temp_ca.crt ssl/temp_ca_signed.crt
	rm -rf $(SSLDIRS)

clean distclean maintainer-clean:
	rm -rf tmp_check
	rm -rf ssl/*.old ssl/new_certs_dir ssl/client*_tmp.key

# Doesn't depend on $(SSLFILES) because we don't rebuild them by default
check:
	$(prove_check)

installcheck:
	$(prove_installcheck)
