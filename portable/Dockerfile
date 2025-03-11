ENV SDKURL https://github.com/pygame-web/portable-sdk/releases/download/3.1.74.7bi/python3.13-wasm-sdk-alpine-3.21.tar.lz4
ENV SDKROOT /tmp/sdk
ENV PIP_ROOT_USER_ACTION ignore
ENV PIP_NO_CACHE_DIR 1


# nb: the python for build from sdk built with clang directly is used as SYS_PYTHON for emsdk
RUN apk add --no-cache --virtual .build-deps \
	    tar file lz4 \
	    git patch bison flex \
        findutils binutils coreutils \
	    libffi curl perl nodejs \
        make autoconf automake libtool pkgconfig \
    ;

WORKDIR /workspace

RUN ./wasm-build.sh contrib extra
