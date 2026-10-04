# Build environment for ARMX360 / XenDroid.
#
# WHY THIS EXISTS. BUILD.md documents a bare-host `./gradlew` flow, and CI builds
# in a container, but there was no way to reproduce that container locally: a
# bare machine with no JDK, NDK or SPIR-V tools fails in three separate places,
# the least obvious being CMakeLists.txt:64-68, which raises FATAL_ERROR for a
# missing glslangValidator while it looks like a C++ toolchain problem.
#
# This image is the CI environment, so "works in CI" and "works here" mean the
# same thing. Base image is deliberately the SAME one the workflow uses
# (.github/workflows/ARMX360.yml) rather than a hand-rolled one, so the two
# cannot drift. Verified 2026-10-04; that pull reported digest
# sha256:c724009e305b4607157287624033ab97f319af44c244bfc9f73b6293f3bb01b9.
# Pinned to the tag, not the digest, on purpose: CI uses the tag, and pinning
# here would let the two quietly diverge.
FROM ghcr.io/cirruslabs/android-sdk:35

# Versions come from one place and match the CI workflow's env block.
# The base image already provides JDK 21 and build-tools 35.0.0; it does NOT
# provide the NDK, CMake, or any of the three SPIR-V tools.
ARG ANDROID_PLATFORM=android-35
ARG ANDROID_NDK_VERSION=29.0.14206865
ARG ANDROID_CMAKE_VERSION=3.30.3
ARG BUILD_TOOLS_VERSION=35.0.0

# glslang-tools/spirv-tools supply the shader toolchain gen_android_spirv.py
# needs at configure time; ninja-build is what the SDK's CMake uses to drive the
# native build; python3 runs that generator.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates \
      git \
      ninja-build \
      python3 \
      glslang-tools \
      spirv-tools \
 && rm -rf /var/lib/apt/lists/*

# ANDROID_HOME/ANDROID_SDK_ROOT already point at /opt/android-sdk-linux in the
# base image and cmdline-tools is already on PATH; set them anyway so the image
# also works if that ever changes.
ENV ANDROID_HOME=/opt/android-sdk-linux
ENV ANDROID_SDK_ROOT=/opt/android-sdk-linux
ENV PATH=${PATH}:${ANDROID_HOME}/cmdline-tools/latest/bin

# Licences are normally accepted interactively; do it once, non-interactively,
# or sdkmanager exits non-zero and every later RUN fails for the wrong reason.
RUN yes | sdkmanager --licenses > /dev/null || true

RUN sdkmanager --install \
      "platform-tools" \
      "platforms;${ANDROID_PLATFORM}" \
      "build-tools;${BUILD_TOOLS_VERSION}" \
      "ndk;${ANDROID_NDK_VERSION}" \
      "cmake;${ANDROID_CMAKE_VERSION}"

# Fail the BUILD if the toolchain is incomplete, rather than letting the image
# look fine and then failing ~4 minutes into a native compile. This mirrors the
# configure-time check in emulator-core/src/main/cpp/CMakeLists.txt:64-68, at
# image-build time instead.
RUN set -eu; \
    for t in glslangValidator spirv-opt spirv-dis; do \
        command -v "$t" > /dev/null || { echo "missing SPIR-V tool: $t" >&2; exit 1; }; \
    done; \
    [ -d "${ANDROID_HOME}/ndk/${ANDROID_NDK_VERSION}" ] || { echo "missing NDK" >&2; exit 1; }; \
    [ -x "${ANDROID_HOME}/cmake/${ANDROID_CMAKE_VERSION}/bin/cmake" ] || { echo "missing CMake" >&2; exit 1; }; \
    java -version; \
    echo "toolchain OK: NDK ${ANDROID_NDK_VERSION}, CMake ${ANDROID_CMAKE_VERSION}"

# The source tree is NOT copied in. Bind-mount it instead: this repo is >1 GB
# with submodules, and copying it would invalidate the layer on every source
# edit. See BUILD.md for the docker command.
WORKDIR /src