# SPDX-License-Identifier: MIT
# Copyright (c) 2023 University of Washington

FROM ubuntu:22.04

ENV DRV_ROOT /install

RUN apt update -y \
        && apt install -y \
        make \
        build-essential \
        openssh-client \
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
        && git clone https://github.com/mrutt92/sst-core.git -b devel-drv-changes /sst-core-src \
        && cd /sst-core-src \
        && ./autogen.sh \
        && mkdir -p /sst-core-build \
        && mkdir -p ${DRV_ROOT} \
        && cd /sst-core-build \
        && /sst-core-src/configure --prefix=${DRV_ROOT} \
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
        && git clone https://github.com/mrutt92/sst-elements.git -b devel-drv-changes /sst-elements-src \
        && cd /sst-elements-src \
        && git checkout devel-drv-changes \
        && ./autogen.sh \
        && mkdir -p /sst-elements-build \
        && cd /sst-elements-build \
        && /sst-elements-src/configure \
        --prefix=${DRV_ROOT} \
        --with-sst-core=${DRV_ROOT} \
        && make -j `nproc` install \
        && rm -rf /sst-elements-build \
        && apt remove -y git libltdl-dev \
        && apt clean -y

# install boost
RUN apt update -y \
        && apt install -y \
        wget \
        && mkdir -p /downloads \
        && cd /downloads \
	&& wget https://archives.boost.io/release/1.89.0/source/boost_1_89_0.tar.bz2 \
        && tar xf boost_1_89_0.tar.bz2 \
        && cd /downloads/boost_1_89_0 \
        && /downloads/boost_1_89_0/bootstrap.sh \
        && /downloads/boost_1_89_0/b2 --prefix=${DRV_ROOT} install \
        && rm -rf /downloads \
        && apt remove -y wget \
        && apt clean -y

RUN mkdir ~/.ssh
ARG ssh_prv_key
ARG ssh_pub_key
RUN echo "$ssh_prv_key" > /root/.ssh/id_ed25519 && \
    echo "$ssh_pub_key" > /root/.ssh/id_ed25519.pub && \
    chmod 600 /root/.ssh/id_ed25519 && \
    chmod 600 /root/.ssh/id_ed25519.pub && \
    ssh-keyscan github.com >> /root/.ssh/known_hosts

RUN eval "$(ssh-agent -s)" && \
    ssh-add ~/.ssh/id_ed25519

RUN apt update -y \
        && apt install -y git automake gawk wget curl texinfo libgmp-dev flex bison \
        && git clone https://github.com/Alan-S-Andrade/riscv-gnu-toolchain.git -b pandodrvsim /riscv-gnu-toolchain \
        && cd /riscv-gnu-toolchain \
        && ./configure --enable-multilib --with-arch=rv64imafd --disable-linux --prefix=${DRV_ROOT} \
        && make -j `nproc` \
        && make install \
        && rm -rf /riscv-gnu-toolchain \
        && apt remove -y git automake gawk wget curl texinfo libgmp-dev flex bison \
        && apt clean -y

# install other dependencies
RUN apt update -y \
        && apt install -y \
        git \
        clang \
        libopenmpi-dev \
        bc \
        python3-pip \
        wget \
        && apt clean -y

RUN pip3 install gdown numpy

RUN mkdir -p cmake-src \
	&& cd cmake-src \
	&& wget https://github.com/Kitware/CMake/releases/download/v4.2.1/cmake-4.2.1.tar.gz \
	&& tar xf cmake-4.2.1.tar.gz \
	&& cd cmake-4.2.1 \
	&& apt install -y openssl libssl-dev \
	&& ./bootstrap --prefix=/usr/local --parallel=`nproc` \
	&& make -j `nproc` \
	&& make -j `nproc` install 

ENV PATH "${PATH}":${DRV_ROOT}/bin
ENV PYTHONPATH "/root/drv/tests:${PYTHONPATH}"
ENV LD_LIBRARY_PATH ${DRV_ROOT}/lib
ENV LIBRARY_PATH ${DRV_ROOT}/lib
ENV C_INCLUDE_PATH ${DRV_ROOT}/include
ENV CPLUS_INCLUDE_PATH ${DRV_ROOT}/include
ENV OMPI_CXX clang++

# copy drv sources
COPY  . /root/drv/
WORKDIR /root/drv/

# build drv
RUN rm -rf /root/drv/build \
        && mkdir -p /root/drv/build \
        && cd /root/drv/build \
	&& cmake .. -DSST_CORE_PREFIX=${DRV_ROOT} -DSST_ELEMENTS_PREFIX=${DRV_ROOT} -DGNU_RISCV_TOOLCHAIN_PREFIX=${DRV_ROOT}
