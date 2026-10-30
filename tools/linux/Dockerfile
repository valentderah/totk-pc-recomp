# Build environment for the Linux setup AppImage. One pinned image, so the artifact does not depend on the builder's
# distro: Ubuntu 24.04 gives the same python, binutils and zsync everywhere, .NET 10 publishes the NativeAOT shader
# translator, and appimagetool and the type-2 runtime are pinned by hash by tools/make_release_linux.py itself.
#
# The game and the runtime library are compiled against the Steam Runtime sniper sysroot that the release downloads,
# so this image only has to run the release script and the translator build -- it is not what decides the glibc floor
# of the result.
#
#   docker build -f tools/linux/Dockerfile -t totk-linux-release tools/linux
#   python3 tools/make_release_linux.py --ci-container --out build/release-linux --appimage <path>
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        ca-certificates curl git python3 python3-venv python3-pip \
        build-essential clang lld ninja-build pkg-config binutils file \
        zsync imagemagick desktop-file-utils python3-pil \
        libvulkan1 mesa-vulkan-drivers xvfb \
    && rm -rf /var/lib/apt/lists/*

# .NET 10 SDK: build_xlate.py publishes totk_xlate for linux-x64 with NativeAOT.
RUN curl -fsSL https://dot.net/v1/dotnet-install.sh -o /tmp/dotnet-install.sh \
    && bash /tmp/dotnet-install.sh --channel 10.0 --install-dir /opt/dotnet \
    && ln -sf /opt/dotnet/dotnet /usr/local/bin/dotnet \
    && rm -f /tmp/dotnet-install.sh
ENV DOTNET_ROOT=/opt/dotnet
ENV PATH=/opt/dotnet:$PATH
ENV DOTNET_CLI_TELEMETRY_OPTOUT=1 DOTNET_NOLOGO=1

WORKDIR /work
