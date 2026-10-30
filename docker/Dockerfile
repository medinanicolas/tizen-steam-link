# Toolchain image: Tizen Studio 5.6 web CLI (packaging, signing, sdb) and Samsung's
# Emscripten 1.39.4.7 fork (WASM Player and Sockets Extension headers). Same versions
# Moonlight for Tizen builds with. The source tree is mounted at run time, see
# scripts/build-tizen.sh.
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
ARG UID=1000
ARG GID=1000
ENV TZ=Etc/UTC

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        cmake \
        expect \
        git \
        ninja-build \
        patch \
        python2 \
        python3 \
        unzip \
        wget \
        zip \
    && rm -rf /var/lib/apt/lists/* \
    && ln -sf /usr/bin/python2 /usr/bin/python

# The host's GID may already exist in the image (macOS users are in staff, GID 20, which
# is dialout in Ubuntu), so reuse it instead of failing.
RUN (getent group ${GID} >/dev/null || groupadd -g ${GID} tsl) && useradd -m -u ${UID} -g ${GID} -s /bin/bash tsl
USER tsl
WORKDIR /home/tsl

RUN wget -nv -O web-cli.bin \
        'https://download.tizen.org/sdk/Installer/tizen-studio_5.6/web-cli_Tizen_Studio_5.6_ubuntu-64.bin' \
    && chmod +x web-cli.bin \
    && ./web-cli.bin --accept-license /home/tsl/tizen-studio \
    && rm web-cli.bin
ENV PATH=/home/tsl/tizen-studio/tools/ide/bin:/home/tsl/tizen-studio/tools:${PATH}

# Throwaway author certificate. Tizen 7+ TVs need the package re-signed with a Samsung
# certificate for the TV's DUID, which Apps2Samsung does at install time.
RUN tizen certificate -a TSL -f TSL -p 1234 \
    && tizen security-profiles add -n TSL -a /home/tsl/tizen-studio-data/keystore/author/TSL.p12 -p 1234 \
    && sed -i 's|/home/tsl/tizen-studio-data/keystore/author/TSL.pwd||' \
        /home/tsl/tizen-studio-data/profile/profiles.xml \
    && sed -i 's|/home/tsl/tizen-studio-data/tools/certificate-generator/certificates/distributor/tizen-distributor-signer.pwd|tizenpkcs12passfordsigner|' \
        /home/tsl/tizen-studio-data/profile/profiles.xml

RUN wget -nv -O emscripten.zip 'https://developer.samsung.com/smarttv/file/a5013a65-af11-4b59-844f-2d34f14d19a9' \
    && unzip -q emscripten.zip \
    && rm emscripten.zip \
    && cd emscripten-release-bundle/emsdk \
    && ./emsdk activate latest-fastcomp
ENV TSL_EMSCRIPTEN_TOOLCHAIN=/home/tsl/emscripten-release-bundle/emsdk/fastcomp/emscripten/cmake/Modules/Platform/Emscripten.cmake

WORKDIR /src
