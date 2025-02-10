#!/bin/bash

DEPS=(
    # Setup
    wget
    gpg
    # Build
    bison
    docbook-xsl
    flex
    gettext
    libkrb5-dev
    libldap2-dev
    liblz4-dev
    libpam0g-dev
    libperl-dev
    libreadline6-dev
    libselinux1-dev
    libssl-dev
    libsystemd-dev
    libxml2
    libxml2-dev
    libxml2-utils
    libxslt-dev
    meson
    ninja-build
    pkg-config
    python3-dev
    systemtap-sdt-dev
    tcl-dev
    uuid-dev
    xsltproc
    zlib1g-dev
    # Build pg_tde
    libcurl4-openssl-dev
    # Test
    libipc-run-perl
    # Test pg_tde
    python3-pykmip
    libhttp-server-simple-perl
)

sudo apt-get update
sudo apt-get install -y ${DEPS[@]}

bash -c "$(wget -O - https://apt.llvm.org/llvm.sh)"

# Vault
wget -O- https://apt.releases.hashicorp.com/gpg | sudo gpg --dearmor -o /usr/share/keyrings/hashicorp-archive-keyring.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/hashicorp-archive-keyring.gpg] https://apt.releases.hashicorp.com $(lsb_release -cs) main" | sudo tee /etc/apt/sources.list.d/hashicorp.list
sudo apt-get update && sudo apt-get install -y vault
