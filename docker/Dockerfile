FROM ubuntu:22.04

RUN apt update -y \
        && apt install -y \
        make \
        build-essential \
        && apt clean -y

# install sst core
RUN apt update -y \
        && apt install -y \
        libopenmpi-dev \
        openmpi-bin \
        openmpi-common \
        libtool \
        libtool-bin \
        autoconf \
        python3 \
        python3-dev \
        automake \
        git \
        && git clone https://github.com/sstsimulator/sst-core.git /sst-core-src \
        && cd /sst-core-src \
        && ./autogen.sh \
        && mkdir -p /sst-core-build \
        && mkdir -p /install \
        && cd /sst-core-build \
        && /sst-core-src/configure --prefix=/install \
        && make -j `nproc` install \
        && rm -rf /sst-core-src \
        && rm -rf /sst-core-build \
        && apt remove -y git automake \
        && apt clean -y

# install sst-elements
RUN apt update -y \
        && apt install -y \
        git \
        libltdl-dev \
        && git clone https://github.com/mrutt92/sst-elements.git /sst-elements-src \
        && cd /sst-elements-src \
        && git checkout devel-drv-changes \
        && ./autogen.sh \
        && mkdir -p /sst-elements-build \
        && cd /sst-elements-build \
        && /sst-elements-src/configure \
        --prefix=/install \
        --with-sst-core=/install \
        && make -j `nproc` install \
        && rm -rf /sst-elements-build \
        && apt remove -y git libltdl-dev \
        && apt clean -y

# build drv

ENV PATH "${PATH}":/install/bin
ENV LD_LIBRARY_PATH /install/lib
ENV LIBRARY_PATH /install/lib

