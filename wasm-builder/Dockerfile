FROM debian:12 AS build_sdk

ARG PG_VERSION=17.5
ARG SDK_VERSION=3.1.74.11bi-w-n
ARG DEBUG=false
ARG OBJDUMP=true
ARG PG_BRANCH=REL_17_5_WASM-pglite

ENV \
  PG_VERSION=$PG_VERSION \
  SDK_VERSION=$SDK_VERSION \
  SDKROOT=/tmp/sdk \
  SYS_PYTHON=/usr/bin/python3 \
  DEBUG=$DEBUG \
  BUILDS=3.12 \
  EMFLAVOUR=3.1.74

RUN apt-get update && \
  apt-get install -y git wget lz4 bzip2 pv bash python3 build-essential libreadline-dev zlib1g-dev bison flex xz-utils perl

RUN apt-get install -y clang autoconf wget curl lz4 lsb-release zlib1g-dev libssl-dev 

RUN bash -c "wget -qO- https://github.com/electric-sql/portable-sdk/releases/download/3.1.74.12.0/python3.13-wasm-sdk-debian12-$(arch).tar.lz4 | lz4 -d | tar -xvf -"
