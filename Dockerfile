#
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
#

#
# Dockerfile for guacamole-server
#

# The Alpine Linux image that should be used as the basis for the guacd image
# NOTE: Using 3.18 because the required openssl1.1-compat-dev package was
# removed in more recent versions.
ARG ALPINE_BASE_IMAGE=3.18

# The target architecture of the build. Valid values are "ARM" and "X86". By
# default, this is detected automatically.
ARG BUILD_ARCHITECTURE

# Populated automatically by docker buildx for each platform in a
# multi-platform build (e.g. TARGETARCH=amd64, TARGETVARIANT=v3 for
# linux/amd64/v3). autobuild.sh reads these to pick an appropriate
# -march flag so every compute-heavy library is specialized for the
# microarchitecture level the resulting image will be published as.
# When unset (manual `docker build` without buildx), autobuild.sh falls
# back to the build host's `uname -m` and uses baseline flags.
ARG TARGETARCH
ARG TARGETVARIANT

# The number of processes that may run simultaneously during the build. By
# default, this is detected automatically.
ARG BUILD_JOBS

# The directory that will house the guacamole-server source during the build 
ARG BUILD_DIR=/tmp/guacamole-server

# FreeRDP version (default to version 2)
ARG FREERDP_VERSION=2

# The final install location for guacamole-server and all dependencies. NOTE:
# This value is hard-coded in the entrypoint. Any change to this value must be
# propagated there.
ARG PREFIX_DIR=/opt/guacamole

#
# Automatically select the latest versions of each core protocol support
# library (these can be overridden at build time if a specific version is
# needed)
#
ARG WITH_FREERDP="${FREERDP_VERSION}(\.\d+)+"
ARG WITH_LIBSSH2='libssh2-\d+(\.\d+)+'
ARG WITH_LIBTELNET='\d+(\.\d+)+'
ARG WITH_LIBVNCCLIENT='LibVNCServer-\d+(\.\d+)+'
ARG WITH_LIBWEBP='v\d+(\.\d+)+'
ARG WITH_LIBWEBSOCKETS='v\d+(\.\d+)+'
ARG WITH_ZLIB_NG='\d+(\.\d+)+'

#
# Default build options for each core protocol support library, as well as
# guacamole-server itself (these can be overridden at build time if different
# options are needed)
#

ARG FREERDP_ARM_OPTS=""

ARG FREERDP_OPTS="\
    -DBUILTIN_CHANNELS=OFF \
    -DCHANNEL_URBDRC=OFF \
    -DWITH_ALSA=OFF \
    -DWITH_CAIRO=ON \
    -DWITH_CHANNELS=ON \
    -DWITH_CLIENT=ON \
    -DWITH_CUPS=OFF \
    -DWITH_DIRECTFB=OFF \
    -DWITH_FFMPEG=OFF \
    -DWITH_FUSE=OFF \
    -DWITH_GSM=OFF \
    -DWITH_GSSAPI=OFF \
    -DWITH_IPP=OFF \
    -DWITH_JPEG=ON \
    -DWITH_KRB5=ON \
    -DWITH_LIBSYSTEMD=OFF \
    -DWITH_MANPAGES=OFF \
    -DWITH_OPENH264=OFF \
    -DWITH_OPENSSL=ON \
    -DWITH_OSS=OFF \
    -DWITH_PCSC=OFF \
    -DWITH_PKCS11=OFF \
    -DWITH_PULSE=OFF \
    -DWITH_SERVER=OFF \
    -DWITH_SERVER_INTERFACE=OFF \
    -DWITH_SHADOW_MAC=OFF \
    -DWITH_SHADOW_X11=OFF \
    -DWITH_SWSCALE=OFF \
    -DWITH_WAYLAND=OFF \
    -DWITH_X11=OFF \
    -DWITH_X264=OFF \
    -DWITH_XCURSOR=ON \
    -DWITH_XEXT=ON \
    -DWITH_XI=OFF \
    -DWITH_XINERAMA=OFF \
    -DWITH_XKBFILE=ON \
    -DWITH_XRENDER=OFF \
    -DWITH_XTEST=OFF \
    -DWITH_XV=OFF \
    -DWITH_ZLIB=ON"

ARG FREERDP_X86_OPTS=""

ARG GUACAMOLE_SERVER_ARM_OPTS=""

ARG GUACAMOLE_SERVER_OPTS="\
    --disable-guaclog \
    CPPFLAGS=-Wno-error=deprecated-declarations"

ARG GUACAMOLE_SERVER_X86_OPTS=""

ARG LIBSSH2_ARM_OPTS=""

ARG LIBSSH2_OPTS="\
    -DBUILD_EXAMPLES=OFF \
    -DBUILD_SHARED_LIBS=ON"

ARG LIBSSH2_X86_OPTS=""

ARG LIBTELNET_ARM_OPTS=""

ARG LIBTELNET_OPTS="\
    --disable-static \
    --disable-util"

ARG LIBTELNET_X86_OPTS=""

ARG LIBVNCCLIENT_ARM_OPTS=""

ARG LIBVNCCLIENT_OPTS=""

ARG LIBVNCCLIENT_X86_OPTS=""

ARG LIBWEBP_ARM_OPTS=""

# libwebp is built with everything except the core shared library disabled.
# We consume libwebp programmatically through guac_webp_write() and don't need
# any of the command-line tools, helper libraries, or companion formats.
# WEBP_BUILD_*=OFF keeps the stage narrow, CMake autodetects SIMD features
# (SSE2/SSE4.1/AVX2/NEON) based on the compile target, so nothing extra to
# pass - our autobuild.sh sets CFLAGS=-march=... per TARGETARCH/TARGETVARIANT
# and libwebp picks up the feature macros from that.
ARG LIBWEBP_OPTS="\
    -DBUILD_SHARED_LIBS=ON \
    -DWEBP_BUILD_ANIM_UTILS=OFF \
    -DWEBP_BUILD_CWEBP=OFF \
    -DWEBP_BUILD_DWEBP=OFF \
    -DWEBP_BUILD_EXTRAS=OFF \
    -DWEBP_BUILD_GIF2WEBP=OFF \
    -DWEBP_BUILD_IMG2WEBP=OFF \
    -DWEBP_BUILD_LIBWEBPMUX=OFF \
    -DWEBP_BUILD_VWEBP=OFF \
    -DWEBP_BUILD_WEBPINFO=OFF \
    -DWEBP_BUILD_WEBPMUX=OFF"

ARG LIBWEBP_X86_OPTS=""

ARG LIBWEBSOCKETS_ARM_OPTS=""

ARG LIBWEBSOCKETS_OPTS="\
    -DDISABLE_WERROR=ON \
    -DLWS_WITHOUT_SERVER=ON \
    -DLWS_WITHOUT_TESTAPPS=ON \
    -DLWS_WITHOUT_TEST_CLIENT=ON \
    -DLWS_WITHOUT_TEST_PING=ON \
    -DLWS_WITHOUT_TEST_SERVER=ON \
    -DLWS_WITHOUT_TEST_SERVER_EXTPOLL=ON \
    -DLWS_WITH_STATIC=OFF"

ARG LIBWEBSOCKETS_X86_OPTS=""

ARG ZLIB_NG_ARM_OPTS=""

# zlib-ng is built in compat mode: produces libz.so.1 with the same ABI as
# vanilla zlib, so it becomes a true drop-in replacement at runtime via
# LD_LIBRARY_PATH. Every consumer of DEFLATE in the image (libpng, OpenSSL,
# libwebsockets, FreeRDP, ...) transparently picks up SIMD-accelerated
# compress/decompress with no source changes.
#
# Tests and gtest are disabled since they're not meaningful for the runtime
# image. CMake autodetects SIMD features (SSE2/SSSE3/PCLMUL/AVX2/AVX-512)
# from the compile target - autobuild.sh's CFLAGS=-march=... from
# TARGETARCH/TARGETVARIANT drives the selection.
ARG ZLIB_NG_OPTS="\
    -DZLIB_COMPAT=ON \
    -DZLIB_ENABLE_TESTS=OFF \
    -DWITH_GTEST=OFF \
    -DBUILD_SHARED_LIBS=ON"

ARG ZLIB_NG_X86_OPTS=""

#
# Base builder image that will be used by subsequent build stages, including
# for building dependencies of guacamole-server.
#

FROM alpine:${ALPINE_BASE_IMAGE} AS builder
ARG BUILD_DIR

# Install build dependencies. libwebp is intentionally omitted - we build it
# ourselves from source in the libwebp stage below so the AVX2 encoder
# paths introduced in libwebp 1.4 (and later) activate under the
# appropriate -march for the target platform, which Alpine 3.18's
# libwebp-dev (1.3.x) predates. zlib-dev is retained here only to satisfy
# build-time header requirements of other apk packages - the runtime libz
# is overridden to our from-source zlib-ng (compat mode) via
# LD_LIBRARY_PATH.
RUN apk add --no-cache                \
        autoconf                      \
        automake                      \
        build-base                    \
        cairo-dev                     \
        cjson-dev                     \
        cmake                         \
        cunit-dev                     \
        git                           \
        grep                          \
        krb5-dev                      \
        libjpeg-turbo-dev             \
        libpng-dev                    \
        libtool                       \
        make                          \
        openssl1.1-compat-dev         \
        pango-dev                     \
        pulseaudio-dev                \
        sdl2-dev                      \
        sdl2_ttf-dev                  \
        util-linux-dev                \
        webkit2gtk-dev

# Copy generic, automatic build script
COPY ./src/guacd-docker/bin/autobuild.sh ${BUILD_DIR}/src/guacd-docker/bin/

#
# Build dependency: zlib-ng
#
# Compiled in compat mode (libz.so.1), so it becomes the runtime libz for
# every consumer of DEFLATE in the final image once LD_LIBRARY_PATH prefers
# /opt/guacamole/lib over /usr/lib. CMake picks appropriate SIMD paths
# (SSE2/SSSE3/PCLMUL/AVX2/AVX-512 on x86, NEON on ARM) based on the
# -march= autobuild.sh derives from TARGETARCH/TARGETVARIANT.
#

FROM builder AS zlib-ng
ARG BUILD_DIR
ARG TARGETARCH
ARG TARGETVARIANT
ARG PREFIX_DIR
ARG WITH_ZLIB_NG
ARG ZLIB_NG_ARM_OPTS
ARG ZLIB_NG_OPTS
ARG ZLIB_NG_X86_OPTS

RUN ${BUILD_DIR}/src/guacd-docker/bin/autobuild.sh "ZLIB_NG" \
    "https://github.com/zlib-ng/zlib-ng"

#
# Build dependency: libwebp
#
# Built from source (rather than taken from apk) so libwebp 1.4+ AVX2
# encoder paths activate under the appropriate -march= for the target
# platform variant. Alpine 3.18 ships libwebp 1.3.x, which predates AVX2.
#

FROM builder AS libwebp
ARG BUILD_DIR
ARG TARGETARCH
ARG TARGETVARIANT
ARG LIBWEBP_ARM_OPTS
ARG LIBWEBP_OPTS
ARG LIBWEBP_X86_OPTS
ARG PREFIX_DIR
ARG WITH_LIBWEBP

RUN ${BUILD_DIR}/src/guacd-docker/bin/autobuild.sh "LIBWEBP" \
    "https://github.com/webmproject/libwebp"

#
# Build dependency: libssh2
#

FROM builder AS libssh2
ARG BUILD_DIR
ARG TARGETARCH
ARG TARGETVARIANT
ARG LIBSSH2_ARM_OPTS
ARG LIBSSH2_OPTS
ARG LIBSSH2_X86_OPTS
ARG PREFIX_DIR
ARG WITH_LIBSSH2

# libssh2 uses zlib for its compression support. Pull in zlib-ng so
# libssh2's find_package(ZLIB) picks up our SIMD-accelerated build.
COPY --from=zlib-ng ${PREFIX_DIR} ${PREFIX_DIR}

RUN ${BUILD_DIR}/src/guacd-docker/bin/autobuild.sh "LIBSSH2" \
    "https://github.com/libssh2/libssh2"

#
# Build dependency: libtelnet
#

FROM builder AS libtelnet
ARG BUILD_DIR
ARG TARGETARCH
ARG TARGETVARIANT
ARG LIBTELNET_ARM_OPTS
ARG LIBTELNET_OPTS
ARG LIBTELNET_X86_OPTS
ARG PREFIX_DIR
ARG WITH_LIBTELNET

RUN ${BUILD_DIR}/src/guacd-docker/bin/autobuild.sh "LIBTELNET" \
    "https://github.com/seanmiddleditch/libtelnet"

#
# Build dependency: libvncclient
#

FROM builder AS libvncclient
ARG BUILD_DIR
ARG TARGETARCH
ARG TARGETVARIANT
ARG LIBVNCCLIENT_ARM_OPTS
ARG LIBVNCCLIENT_OPTS
ARG LIBVNCCLIENT_X86_OPTS
ARG PREFIX_DIR
ARG WITH_LIBVNCCLIENT

# libvncclient uses zlib for the Tight encoding and libwebp for
# WebP-over-VNC updates, so ensure libvncclient's build finds our
# from-source versions rather than Alpine's packages.
COPY --from=zlib-ng ${PREFIX_DIR} ${PREFIX_DIR}
COPY --from=libwebp ${PREFIX_DIR} ${PREFIX_DIR}

RUN ${BUILD_DIR}/src/guacd-docker/bin/autobuild.sh "LIBVNCCLIENT" \
    "https://github.com/LibVNC/libvncserver"

#
# Build dependency: libwebsockets
#

FROM builder AS libwebsockets
ARG BUILD_DIR
ARG TARGETARCH
ARG TARGETVARIANT
ARG LIBWEBSOCKETS_ARM_OPTS
ARG LIBWEBSOCKETS_OPTS
ARG LIBWEBSOCKETS_X86_OPTS
ARG PREFIX_DIR
ARG WITH_LIBWEBSOCKETS

# libwebsockets uses zlib for permessage-deflate, so pull in zlib-ng to
# benefit from SIMD-accelerated compression on every WebSocket frame that
# negotiates compression.
COPY --from=zlib-ng ${PREFIX_DIR} ${PREFIX_DIR}

RUN ${BUILD_DIR}/src/guacd-docker/bin/autobuild.sh "LIBWEBSOCKETS" \
    "https://github.com/warmcat/libwebsockets"

#
# Build dependency: FreeRDP
#

FROM builder AS freerdp
ARG BUILD_DIR
ARG TARGETARCH
ARG TARGETVARIANT
ARG FREERDP_ARM_OPTS
ARG FREERDP_OPTS
ARG FREERDP_X86_OPTS
ARG PREFIX_DIR
ARG WITH_FREERDP

# FreeRDP uses zlib (WITH_ZLIB=ON) for channel compression and libwebp for
# some bitmap codecs. Both of these come from our from-source builds so
# they pick up SIMD-accelerated codegen for the target platform variant.
COPY --from=zlib-ng ${PREFIX_DIR} ${PREFIX_DIR}
COPY --from=libwebp ${PREFIX_DIR} ${PREFIX_DIR}

RUN ${BUILD_DIR}/src/guacd-docker/bin/autobuild.sh "FREERDP" \
    "https://github.com/FreeRDP/FreeRDP"

#
# STAGE 7: Collect dependencies built by previous stages and build
# guacamole-server.
#

FROM builder AS guacamole-server
ARG BUILD_DIR
ARG TARGETARCH
ARG TARGETVARIANT
ARG FREERDP_VERSION
ARG GUACAMOLE_SERVER_ARM_OPTS
ARG GUACAMOLE_SERVER_OPTS
ARG GUACAMOLE_SERVER_X86_OPTS
ARG PREFIX_DIR

# Copy dependencies built in previous stages. Order matters only for
# overlapping files; each stage writes disjoint subsets of PREFIX_DIR
# so the composition is idempotent regardless of order. zlib-ng and
# libwebp come first since their libraries/headers are consumed by
# the guacamole-server build (libguac links libwebp for encoding,
# libpng/freerdp/etc. pick up libz.so.1 via LD_LIBRARY_PATH at runtime
# and via the linker search path at link time).
COPY --from=zlib-ng ${PREFIX_DIR} ${PREFIX_DIR}
COPY --from=libwebp ${PREFIX_DIR} ${PREFIX_DIR}
COPY --from=freerdp ${PREFIX_DIR} ${PREFIX_DIR}
COPY --from=libssh2 ${PREFIX_DIR} ${PREFIX_DIR}
COPY --from=libtelnet ${PREFIX_DIR} ${PREFIX_DIR}
COPY --from=libvncclient ${PREFIX_DIR} ${PREFIX_DIR}
COPY --from=libwebsockets ${PREFIX_DIR} ${PREFIX_DIR}

# Use guacamole-server source from build context
COPY . ${BUILD_DIR}

RUN ${BUILD_DIR}/src/guacd-docker/bin/autobuild.sh "GUACAMOLE_SERVER" "${BUILD_DIR}"

# Determine location of the FREERDP library based on the version.
ARG FREERDP_LIB_PATH=${PREFIX_DIR}/lib/freerdp${FREERDP_VERSION}

# Record the packages of all runtime library dependencies
RUN ${BUILD_DIR}/src/guacd-docker/bin/list-dependencies.sh \
        ${PREFIX_DIR}/sbin/guacd               \
        ${PREFIX_DIR}/lib/libguac-client-*.so  \
        ${FREERDP_LIB_PATH}/*guac*.so   \
        > ${PREFIX_DIR}/DEPENDENCIES

#
# STAGE 8: Final, runtime image.
#

# Use same Alpine version as the base for the runtime image
FROM alpine:${ALPINE_BASE_IMAGE} AS runtime
ARG PREFIX_DIR

# Copy build artifacts into this stage
COPY --from=guacamole-server ${PREFIX_DIR} ${PREFIX_DIR}

# Bring runtime environment up to date and install runtime dependencies
RUN apk add --no-cache                \
        ca-certificates               \
        font-noto-cjk                 \
        ghostscript                   \
        netcat-openbsd                \
        shadow                        \
        terminus-font                 \
        ttf-dejavu                    \
        ttf-liberation                \
        util-linux-login && \
    xargs apk add --no-cache < ${PREFIX_DIR}/DEPENDENCIES

# Runtime environment
ENV LC_ALL=C.UTF-8
ENV LD_LIBRARY_PATH=${PREFIX_DIR}/lib

# Checks the operating status every 5 minutes with a timeout of 5 seconds
HEALTHCHECK --interval=5m --timeout=5s CMD nc -z 127.0.0.1 4822 || exit 1

# Create a new user guacd
ARG UID=1000
ARG GID=1000
RUN groupadd --gid $GID guacd
RUN useradd --system --create-home --shell /sbin/nologin --uid $UID --gid $GID guacd

# Run with user guacd
USER guacd

# Expose the default listener port
EXPOSE 4822

COPY ./src/guacd-docker/bin/entrypoint.sh /opt/guacamole/
ENTRYPOINT [ "/opt/guacamole/entrypoint.sh" ]
