FROM ubuntu:22.04 AS base

ARG DEBIAN_FRONTEND=noninteractive
ENV TZ=Etc/UTC
RUN apt-get update && apt-get install -y \
    cmake \
    expect \
    git \
    ninja-build \
    python2 \
    unzip \
    wget \
    nodejs \
    npm \
    zip \
    default-jre \
    && rm -rf /var/lib/apt/lists/*

# Some of Samsung scripts make reference to python,
# but Ubuntu only provides /usr/bin/python2.
RUN ln -sf /usr/bin/python2 /usr/bin/python

# Use a non-root user from here on
RUN useradd -m -s /bin/bash moonlight
USER moonlight
WORKDIR /home/moonlight


# Install Tizen Studio
RUN wget -nv -O web-cli_Tizen_Studio_6.1_ubuntu-64.bin 'https://download.tizen.org/sdk/Installer/tizen-studio_6.1/web-cli_Tizen_Studio_6.1_ubuntu-64.bin'
RUN chmod a+x web-cli_Tizen_Studio_6.1_ubuntu-64.bin
RUN ./web-cli_Tizen_Studio_6.1_ubuntu-64.bin --accept-license --no-java-check /home/moonlight/tizen-studio
ENV PATH=/home/moonlight/tizen-studio/tools/ide/bin:/home/moonlight/tizen-studio/tools:${PATH}

# Prepare Tizen signing certificates
RUN tizen certificate \
    -a Moonlight \
    -f Moonlight \
    -p 1234
RUN tizen security-profiles add \
    -n Moonlight \
    -a /home/moonlight/tizen-studio-data/keystore/author/Moonlight.p12 \
    -p 1234

# Workaround to package applications without gnome-keyring
RUN sed -i 's|/home/moonlight/tizen-studio-data/keystore/author/Moonlight.pwd||' /home/moonlight/tizen-studio-data/profile/profiles.xml
RUN sed -i 's|/home/moonlight/tizen-studio-data/tools/certificate-generator/certificates/distributor/tizen-distributor-signer.pwd|tizenpkcs12passfordsigner|' /home/moonlight/tizen-studio-data/profile/profiles.xml

# Install Samsung Emscripten SDK
RUN wget -nv -O emscripten-1.39.4.7-linux64.zip 'https://developer.samsung.com/smarttv/file/a5013a65-af11-4b59-844f-2d34f14d19a9'
RUN unzip emscripten-1.39.4.7-linux64.zip
WORKDIR emscripten-release-bundle/emsdk
RUN ./emsdk activate latest-fastcomp

# Add Java for Emscripten Closure Compiler
RUN echo 'JAVA = "/usr/bin/java"' >> /home/moonlight/.emscripten
WORKDIR ../..

# Build moonlight
COPY --chown=moonlight . ./moonlight-chrome-tizen
RUN cmake \
    -DCMAKE_TOOLCHAIN_FILE=/home/moonlight/emscripten-release-bundle/emsdk/fastcomp/emscripten/cmake/Modules/Platform/Emscripten.cmake \
    -G Ninja \
    -S moonlight-chrome-tizen \
    -B build
RUN cmake --build build
RUN cmake --install build --prefix build

RUN cp moonlight-chrome-tizen/icons/icon.png build/widget/

# Package and sign application
RUN echo \
    'set timeout -1\n' \
    'spawn tizen package -t wgt -- build/widget\n' \
    'expect "Author password:"\n' \
    'send -- "1234\\r"\n' \
    'expect "Yes: (Y), No: (N) ?"\n' \
    'send -- "N\\r"\n' \
    'expect eof\n' \
| expect
RUN mv build/widget/Moonlight.wgt .

# Clone and install wgt-to-usb
RUN git clone https://github.com/fingerartur/wgt-to-usb.git
RUN cd /home/moonlight/wgt-to-usb/ && npm install wgt-to-usb

# Package the application for USB installation
RUN npm exec wgt-to-usb /home/moonlight/Moonlight.wgt
RUN cd /home/moonlight/ && zip -r MoonlightUSB.zip ./userwidget

# Remove unnecessary files
RUN rm -rf \
    build \
    emscripten-1.39.4.7-linux64.zip \
    emscripten-release-bundle \
    moonlight-chrome-tizen \
    tizen-package-expect.sh \
    web-cli_Tizen_Studio_6.1_ubuntu-64.bin \
    .emscripten_cache \
    .emscripten_cache.lock \
    .emscripten_ports \
    .emscripten_sanity \
    .package-manager \
    .wget-hsts

# Use a multistage build to reclaim space from deleted files
FROM ubuntu:22.04
COPY --from=base / /
USER moonlight
WORKDIR /home/moonlight

# Add Tizen Studio to path
ENV PATH=/home/moonlight/tizen-studio/tools/ide/bin:/home/moonlight/tizen-studio/tools:${PATH}
