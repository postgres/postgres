#!/bin/bash

sudo apt update

sudo apt install -y libreadline6-dev systemtap-sdt-dev zlib1g-dev libssl-dev libpam0g-dev bison flex libxml2 libxml2-utils libxml2-dev libxslt-dev xsltproc libkrb5-dev libldap2-dev libsystemd-dev gettext tcl-dev libperl-dev pkg-config libselinux1-dev python3-dev uuid-dev liblz4-dev meson ninja-build gpg wget libcurl4-openssl-dev libxml2-utils docbook-xsl xsltproc

bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)"

# Perl test dependencies

sudo apt install -y libipc-run-perl python3-pykmip libhttp-server-simple-perl

sudo /usr/bin/perl -MCPAN -e 'install IPC::Run'
sudo /usr/bin/perl -MCPAN -e 'install Text::Trim'

# Vault
wget -O- https://apt.releases.hashicorp.com/gpg | sudo gpg --dearmor -o /usr/share/keyrings/hashicorp-archive-keyring.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/hashicorp-archive-keyring.gpg] https://apt.releases.hashicorp.com $(lsb_release -cs) main" | sudo tee /etc/apt/sources.list.d/hashicorp.list
sudo apt update && sudo apt install -y vault