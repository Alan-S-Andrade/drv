SPDX-License-Identifier: MIT
Copyright (c) 2023 University of Washington

# Installing on Your System

Here are the steps to running Drv natively on your system.

## System Requirements

- `clang` >= 11
- `boost` >= 1.82.0
- `openmpi` >= 1.0
- `git`

Drv also requires that sst-core and sst-elements are built and installed.
Here are the repos you should clone with the branches you should checkout.

- `sst-core`: https://github.com/sstsimulator/sst-core (devel)
- `sst-elements`: https://github.com/mrutt92/sst-elements (devel-drv-changes)

Drv can simulate RISCV programs. You will need to have the RISCV toolchain installed.

- `riscv-gnu-toolchain`: https://github.com/riscv-collab/riscv-gnu-toolchain

Use the following command to configure the riscv toolchain:

`./configure --enable-multilib --with-arch=rv64imafd --disable-linux --prefix=<install-path>`

Drv has been tested on CentOS and Ubuntu systems.

## Configuring

In `drv/mk/boost_config.mk`, set `BOOST_INSTALL_DIR` to wherever you installed boost.
That means if you installed `boost` with `bootstrap.sh && b2 --prefix=/path/to/boost/install install`), you should set this
variable to `/path/to/boost/install`.

In `drv/mk/sst_config.mk`, set `SST_ELEMENTS_INSTALL_DIR` to wherever you install sst-elements.
That means if you built `sst-elements` with `configure --prefix=/path/to/sst-elements/install && make install`, you should
set this variable to `/path/to/sst-elements/install`.

In `drv/mk/install_config.mk`, set `DRV_INSTALL_DIR` to wherever you want to install the `DrvAPI` header files and the `Drv` element
libraries needed by `sst`.

In `drv/mk/riscv_config.mk`, set `RISCV_INSTALL_DIR` to wherever you install riscv-gnu-toolchain.
That means if you built `riscv-gnu-toolchain` with `./configure --enable-multilib --with-arch=rv64imafd --disable-linux --prefix=/path/to/toolchain/install`,
you should set this variable to `/path/to/toolchain/install`.


Finally, make sure that wherever you installed the `sst` executable is in your `PATH`.
That means if you built `sst-core`with `configure --prefix=/path/to/sst-core/install && make install`, you should
make sure that `/path/to/sst-core/install/bin` is in your `PATH`.

## Building

Assuming you have configured everything correctly, you should run `make install` from `drv`.

## Running A Drv Application

This requires some understanding of how to use `sst`. 
Please see `sst-elements` for example configuration scripts written in python for using `sst` generally.
Please see `drv/tests/drv-multicore-bus-test.py` and `drv/tests/drv-multicore-nic-test.py` 
for examples of instantiating `Drv` components. 

The primary `Drv` component is `DrvCore` which loads a user program compiled as a dlo (a dynamically loadable object).
See `drv/examples/*` for some example user applications. The rules for building are in `drv/mk/application_common.mk` and 
`drv/mk/application_config.mk`.

`drv/mk/application_common.mk` also has an example rule for running an application that takes no command line arguments.

If you are using `drv-multicore-bus-test.py` or `drv/tests/drv-multicore-nic-test.py` the format for running your 
program with `drv` is the following:

    `sst drv-multicore-{bus|nic}-test.py -- /path/to/my-app/<my-app>.so [ARG1 [ARG2 [... ARGN]]]`

Have fun!

## Running A Drv RISCV Application

Go to `drv/riscv-examples/<some-example>` and run `make run`. This will build the app and run the right model.

# Using Docker

A Dockerfile is provided to start using Drv with minimal pain.
See `docker/Dockerfile`

## Build the Docker Image

`docker build -t <tag> . -f docker/Dockerfile`

## Start the Docker Container
`docker run -dti <tag>`

## Connect to the Docker Container
1. Find the container id with `docker ps`
2. Connect with `docker attach <container-id>`

