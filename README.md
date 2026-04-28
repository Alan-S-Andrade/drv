```
SPDX-License-Identifier: MIT
Copyright (c) 2023 University of Washington
Copyright (c) 2023 Advanced Micro Devices, Inc. All rights reserved.
```

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

## Local build without docker

### Dependencies
```
apt update -y \
        && apt install -y \
        make \
        build-essential \
        openssh-client \
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
        libltdl-dev \
        wget \
        automake \ 
        gawk \
        wget \
        curl \
        texinfo \
        libgmp-dev \
        flex bison
```

### Build dependencies

```
. load_drvx.sh
./build_drvx_deps.sh
```

### Build Drive

`make -j install`

### Test by running an example application

```
cd examples/allocator
make run
```

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

## Running A DrvX Application

DrvX is the PANDO team's fast-functional model for the `PANDOHammer` architecture.
It emulates the programming environment, including the PGAS, the threading model, and the memory system  of `PANDOHammer` hardware.
It runs the application natively.


### Running A DrvX Application
Go to `drv/examples/<some-example>` and run `make run`. This will build the app and run the `PANDOHammerDrvX` model
in `drv/tests/` with the example application.

### Running Older Models
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

## Running A DrvR Application

DrvR is the PANDO team's instruction level simulator for the `PANDOHammer` achitecture.

Go to `drv/riscv-examples/<some-example>` and run `make run`. This will build the app and run the `PANDOHammerDrvR` model
in `drv/tests/` with the example application.

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

docker build \
  --build-arg ssh_prv_key="$(cat ~/.ssh/id_ed25519)" \
  --build-arg ssh_pub_key="$(cat ~/.ssh/id_ed25519.pub)" \
  -t drv:latest . -f docker/Dockerfile
  
docker run -it --rm \
  -v $PWD:/work \
  drv:latest bash


  ## To run it on TACC - Stampede3

  ### 1. SSH + compute node + module

  ```bash
  ssh vineeth_architect@stampede3.tacc.utexas.edu
  # get a compute node (idev or sbatch)
  cdw
  module load tacc-apptainer/1.4.1
  cd /work2/10238/vineeth_architect/stampede3/drv_copy/drv
  ```

  ### 2. Set the paths used by the new bind mounts

  ```bash
  SIF=/work2/10238/vineeth_architect/stampede3/drv_latest.sif
  RAMULATOR_SRC=/work2/10238/vineeth_architect/stampede3/drv_copy/ramulator-build
  RAMULATOR_LIB=$RAMULATOR_SRC/libramulator.so
  RAMULATOR_CONFIGS=$RAMULATOR_SRC/configs
  MEMH_LIB=/work2/10238/vineeth_architect/stampede3/drv-stack/sst-elements/lib/sst-elements-library/libmemHierarchy.so
  ```

  ### 3. BUILD step — apptainer with Ramulator binds

  ```bash
  apptainer exec --cleanenv \
    --env XALT_EXECUTABLE_TRACKING=no \
    --bind "$PWD:/work" \
    --bind "$RAMULATOR_LIB:/install/lib/libramulator.so" \
    --bind "$RAMULATOR_SRC:/tmp/ramulator:ro" \
    "$SIF" \
    bash -lc '
      export RISCV_HOME=/install
      export PATH=/install/bin:$PATH
      cd /work/build_stampede
      cmake .. \
        -DSST_CORE_PREFIX=/install \
        -DSST_ELEMENTS_PREFIX=/install \
        -DGNU_RISCV_TOOLCHAIN_PREFIX=/install \
        -DCMAKE_INSTALL_PREFIX=/work/.local \
        -DSST_ENABLE_RAMULATOR=1 \
        -DRAMULATOR_DIR=/tmp/ramulator
      make -j32 rv64 Drv pandocommand_loader
    '
  ```

  ### 4. RUN step - apptainer with Ramulator + memHierarchy binds

  ```bash
  apptainer exec --cleanenv \
    --env XALT_EXECUTABLE_TRACKING=no \
    --env OMPI_MCA_mtl=^psm2 \
    --bind "$PWD:/work" \
    --bind "$RAMULATOR_LIB:/install/lib/libramulator.so" \
    --bind "$RAMULATOR_CONFIGS:/work/ramulator-configs" \
    --bind "$MEMH_LIB:/install/lib/sst-elements-library/libmemHierarchy.so" \
    "$SIF" \
    bash
  ```

  Run-step additions vs. old README:
  - `--env OMPI_MCA_mtl=^psm2` — disables PSM2 MTL (avoids MPI/OFI noise on Stampede3 fabric).
  - `--bind $RAMULATOR_LIB → /install/lib/libramulator.so` — same override as build.
  - `--bind $RAMULATOR_CONFIGS → /work/ramulator-configs` — the model passes `--dram-backend-config-sliced=/work/ramulator-configs/HBM-pando-16ch.cfg`, so this mount
   is required at runtime.
  - `--bind $MEMH_LIB → /install/lib/sst-elements-library/libmemHierarchy.so` — overrides the container's `libmemHierarchy.so` with your prebuilt one that has
  coherence ALU tagging. Requires the prebuilt `.so` to exist (rebuilt via `rebuild_memhierarchy.sh` if needed).

  ### 5. Inside the container, invoking SST directly via an sbatch script, where you can set architectural parameters and also set the binary.

  e.g.:

  ```bash
  PYTHONPATH=/work/py::/work/model \
    /install/bin/sst -n 1 /work/model/drvr.py -- \
      --with-command-processor=/work/build_stampede/pandocommand/libpandocommand_loader.so \
      --num-pxn=1 --pxn-pods=1 --pod-cores-x=8 --pod-cores-y=8 --core-threads=16 \
      --pod-l2sp-banks=4 --pod-l2sp-interleave=64 \
      --pxn-dram-banks=1 --pxn-dram-cache-size=$((512*1024)) --pxn-dram-cache-slices=4 \
      --dram-backend-config-sliced=/work/ramulator-configs/HBM-pando-16ch.cfg \
      --pxn-dram-cache-alu=0 \
      /work/build_stampede/rv64/drvr/drvr_bfs_csr_shared_queue_baseline \
      --V 1024 --D 16
  ```

  For end-to-end runs, the easiest path is to copy/adapt one of the existing sbatch files (e.g. `bfs_csr_shared_queue.sbatch`) — they wire up build + run + sweep
  loop + summarization in one file.

  ## Fresh setup (user agnostic)

  This section is for setting up DRV on Stampede3 from scratch. Substitute `<tacc-username>` with your TACC username and pick any directory under `$WORK` as your
  root — the example below uses `$WORK/stampede3`.

  ### Step 0 — Workspace

  ```bash
  ssh <tacc-username>@stampede3.tacc.utexas.edu
  cdw                              # cd to $WORK
  mkdir -p $WORK/stampede3 && cd $WORK/stampede3
  module load tacc-apptainer/1.4.1
  ```

  For the rest of this section, `$DRV_ROOT=$WORK/stampede3` is your top-level directory. Set it once:

  ```bash
  export DRV_ROOT=$WORK/stampede3
  ```

  ### Step 1 — Pull the Docker image and convert to a SIF

  Apptainer can pull straight from Docker Hub — no local Docker daemon needed. Don't run this on a login node (image conversion is hefty); use an `idev` session or a
   small `sbatch`.

  ```bash
  # Use a writable cache/tmp inside $WORK so /tmp doesn't fill up
  export APPTAINER_CACHEDIR=$WORK/.apptainer/cache
  export APPTAINER_TMPDIR=$WORK/.apptainer/tmp
  mkdir -p "$APPTAINER_CACHEDIR" "$APPTAINER_TMPDIR"

  # Pull and build the SIF (output is the file every sbatch references)
  cd $DRV_ROOT
  apptainer pull drv_latest.sif docker://alansandrade/drv:latest
  ```

  After this you should have `$DRV_ROOT/drv_latest.sif`.

  > The image is built from `drv/docker/Dockerfile`. It bakes in `sst-core` (`mrutt92/sst-core`, branch `devel-drv-changes`), `sst-elements` (same fork,
  `devel-drv-changes`), Boost 1.89, the RISC-V GNU toolchain, CMake 4.2, plus a stub `libramulator.so` at `/install/lib/`. We override the stub at run time with our
  own builds — see Steps 2 and 3.

  ### Step 2 — Build `libramulator.so` outside the container

  The image still has the Dockerfile's baked-in `libramulator.so`, but we ship our own copy with the SST patches and the HBM-pando configs. Build it once on the host
   (no apptainer needed — it's a plain `g++` build).

  Clone and patch Ramulator at the SHA SST expects:

  ```bash
  cd $DRV_ROOT
  git clone https://github.com/CMU-SAFARI/ramulator.git ramulator-build
  cd ramulator-build
  git checkout 7d2e72306c6079768e11a1867eb67b60cee34a1c
  wget https://github.com/sstsimulator/sst-downloads/releases/download/Patch_Files/ramulator_sha_7d2e723_gcc48Patch.patch
  wget https://github.com/sstsimulator/sst-downloads/releases/download/Patch_Files/ramulator_sha_7d2e723_libPatch.patch
  patch -p1 -i ramulator_sha_7d2e723_gcc48Patch.patch
  patch -p1 -i ramulator_sha_7d2e723_libPatch.patch
  ```

  Then build it:

  ```bash
  make CXX=g++ libramulator.so
  ls libramulator.so configs/HBM-pando-16ch.cfg     # sanity check
  ```

  This `.so` is what the sbatch scripts mount over `/install/lib/libramulator.so` at run time. The `configs/` directory (with `HBM-pando-16ch.cfg`,
  `HBM-pando-32ch.cfg`, etc.) is mounted into the container at `/work/ramulator-configs`.

  **Why two patches?** The Ramulator commit we pin (`7d2e723`) was written for old toolchains and only built a static `.a`. The SST project ships two patches:
  - `gcc48Patch.patch` — fixes lambdas in `Scheduler.h` so it compiles on modern GCC/Clang.
  - `libPatch.patch` — adds a `libramulator.so` Makefile target and `-fPIC` so SST can dlopen it as a memHierarchy backend.

  ### Step 3 — Build `libmemHierarchy.so` with `DRV_CACHE_ALU` (optional)

  The DRAM-cache ALU-tagging changes live in a local sst-elements tree (`$DRV_ROOT/drv-stack/sst-elements`) and must be compiled against the locally-built ramulator.
   A helper script (`rebuild_memhierarchy.sh` in the drv repo) does exactly that — first edit the absolute paths near the top of the script to point at your
  `$DRV_ROOT`, then run:

  ```bash
  cd $DRV_ROOT/drv
  ./rebuild_memhierarchy.sh
  ls $DRV_ROOT/drv-stack/sst-elements/lib/sst-elements-library/libmemHierarchy.so
  ```

  This compiles every `memHierarchy/*.cc` with `-DDRV_CACHE_ALU`, links against `libramulator.so` from Step 2, and writes the `.so` where the sbatch files bind it
  in.

  > **Prereqs the script assumes already exist:** `drv-stack/sst-elements/src/` (sources) and `drv-stack/sst-core/include/` (SST core headers). On a fresh machine,
  clone `mrutt92/sst-core` and `mrutt92/sst-elements` (branch `devel-drv-changes`) into those paths and run `./autogen.sh && ./configure` once, mirroring the
  Dockerfile.

  ### Step 4 — Verify everything is wired

  ```bash
  ls -lh $DRV_ROOT/drv_latest.sif
  ls -lh $DRV_ROOT/ramulator-build/libramulator.so
  ls -lh $DRV_ROOT/ramulator-build/configs/HBM-pando-16ch.cfg
  ls -lh $DRV_ROOT/drv-stack/sst-elements/lib/sst-elements-library/libmemHierarchy.so
  ```

  All four must exist before any of the `*.sbatch` files will work. From here, follow the **"To run it on TACC - Stampede3"** section above (substituting your
  `$DRV_ROOT` for the absolute paths shown there).






  ## Fresh setup (vineeth user perspective example)

  ### Step 0 - Workspace

  ```bash
  ssh vineeth_architect@stampede3.tacc.utexas.edu
  cdw                              # cd to $WORK2
  cd /work2/10238/vineeth_architect/stampede3
  module load tacc-apptainer/1.4.1
  ```

  ### Step 1 - Pull the Docker image and convert to a SIF

  Apptainer can pull straight from Docker Hub — no local Docker daemon needed. Don't run this on a login node (image conversion is hefty); use an `idev` session or a
   small `sbatch`.

  ```bash
  # Use a writable cache/tmp inside $WORK so /tmp doesn't fill up
  export APPTAINER_CACHEDIR=$WORK/.apptainer/cache
  export APPTAINER_TMPDIR=$WORK/.apptainer/tmp
  mkdir -p "$APPTAINER_CACHEDIR" "$APPTAINER_TMPDIR"

  # Pull and build the SIF (output is the file every sbatch references)
  apptainer pull drv_latest.sif docker://alansandrade/drv:latest
  ```

  After this you should have `/work2/10238/vineeth_architect/stampede3/drv_latest.sif`.

  > The image is built from `drv/docker/Dockerfile`. It bakes in `sst-core` (`mrutt92/sst-core`, branch `devel-drv-changes`), `sst-elements` (same fork,
  `devel-drv-changes`), Boost 1.89, the RISC-V GNU toolchain, CMake 4.2, plus a stub `libramulator.so` at `/install/lib/`. We override the stub at run time with our
  own builds — see Steps 2 and 3.

  ### Step 2 - Build `libramulator.so` outside the container

  The image still has the Dockerfile's baked-in `libramulator.so`, but we ship our own copy with the SST patches and the HBM-pando configs. Build it once on the host
   (no apptainer needed — it's a plain `g++` build).

  If `ramulator-build/` is missing on a fresh checkout:

  ```bash
  cd /work2/10238/vineeth_architect/stampede3/drv_copy
  git clone https://github.com/CMU-SAFARI/ramulator.git ramulator-build
  cd ramulator-build
  git checkout 7d2e72306c6079768e11a1867eb67b60cee34a1c
  wget https://github.com/sstsimulator/sst-downloads/releases/download/Patch_Files/ramulator_sha_7d2e723_gcc48Patch.patch
  wget https://github.com/sstsimulator/sst-downloads/releases/download/Patch_Files/ramulator_sha_7d2e723_libPatch.patch
  patch -p1 -i ramulator_sha_7d2e723_gcc48Patch.patch
  patch -p1 -i ramulator_sha_7d2e723_libPatch.patch
  ```

  Then build it:

  ```bash
  cd /work2/10238/vineeth_architect/stampede3/drv_copy/ramulator-build
  make CXX=g++ libramulator.so
  ls libramulator.so configs/HBM-pando-16ch.cfg     # sanity check
  ```

  This `.so` is what the sbatch scripts mount over `/install/lib/libramulator.so` at run time. The `configs/` directory (with `HBM-pando-16ch.cfg`,
  `HBM-pando-32ch.cfg`, etc.) is mounted into the container at `/work/ramulator-configs`.

  **Why two patches?** The Ramulator commit we pin (`7d2e723`) was written for old toolchains and only built a static `.a`. The SST project ships two patches:
  - `gcc48Patch.patch` — fixes lambdas in `Scheduler.h` so it compiles on modern GCC/Clang.
  - `libPatch.patch` — adds a `libramulator.so` Makefile target and `-fPIC` so SST can dlopen it as a memHierarchy backend.

  ### Step 3 - Build `libmemHierarchy.so` with `DRV_CACHE_ALU` (optional)

  The DRAM-cache ALU-tagging changes live in your local sst-elements tree (`drv-stack/sst-elements`) and must be compiled against the locally-built ramulator. A
  helper script does exactly that:

  ```bash
  cd /work2/10238/vineeth_architect/stampede3/drv_copy/drv
  ./rebuild_memhierarchy.sh
  ls /work2/10238/vineeth_architect/stampede3/drv-stack/sst-elements/lib/sst-elements-library/libmemHierarchy.so
  ```

  This compiles every `memHierarchy/*.cc` with `-DDRV_CACHE_ALU`, links against `libramulator.so` from Step 2, and writes the `.so` where the sbatch files bind it
  in.

  > **Prereqs the script assumes already exist:** `drv_stack/sst-elements/src/` (sources) and `drv-stack/sst-core/include/` (SST core headers). On a fresh machine,
  clone `mrutt92/sst-core` and `mrutt92/sst-elements` (branch `devel-drv-changes`) into those paths and run `./autogen.sh && ./configure` once, mirroring the
  Dockerfile.

  ### Step 4 - Verify everything is wired

  ```bash
  ls -lh /work2/10238/vineeth_architect/stampede3/drv_latest.sif
  ls -lh /work2/10238/vineeth_architect/stampede3/drv_copy/ramulator-build/libramulator.so
  ls -lh /work2/10238/vineeth_architect/stampede3/drv_copy/ramulator-build/configs/HBM-pando-16ch.cfg
  ls -lh /work2/10238/vineeth_architect/stampede3/drv-stack/sst-elements/lib/sst-elements-library/libmemHierarchy.so
  ```

  All four must exist before any of the `*.sbatch` files will work.

  ---

  ## Building DRV inside the container

  ```bash
  cd /work2/10238/vineeth_architect/stampede3/drv_copy/drv
  mkdir -p build_stampede

  SIF=/work2/10238/vineeth_architect/stampede3/drv_latest.sif
  RAMULATOR_SRC=/work2/10238/vineeth_architect/stampede3/drv_copy/ramulator-build
  RAMULATOR_LIB=$RAMULATOR_SRC/libramulator.so

  apptainer exec --cleanenv \
    --env XALT_EXECUTABLE_TRACKING=no \
    --bind "$PWD:/work" \
    --bind "$RAMULATOR_LIB:/install/lib/libramulator.so" \
    --bind "$RAMULATOR_SRC:/tmp/ramulator:ro" \
    "$SIF" \
    bash -lc '
      export RISCV_HOME=/install
      export PATH=/install/bin:$PATH
      cd /work/build_stampede
      cmake .. \
        -DSST_CORE_PREFIX=/install \
        -DSST_ELEMENTS_PREFIX=/install \
        -DGNU_RISCV_TOOLCHAIN_PREFIX=/install \
        -DCMAKE_INSTALL_PREFIX=/work/.local \
        -DSST_ENABLE_RAMULATOR=1 \
        -DRAMULATOR_DIR=/tmp/ramulator
      make -j32 rv64 Drv pandocommand_loader
    '
  ```

  Notes vs. the older README:
  - New cmake flags: `-DGNU_RISCV_TOOLCHAIN_PREFIX=/install`, `-DSST_ENABLE_RAMULATOR=1`, `-DRAMULATOR_DIR=/tmp/ramulator`.
  - New binds: host `libramulator.so` over the in-container stub, plus the Ramulator source tree at `/tmp/ramulator` so cmake can find headers.
  - Build targets are now `rv64 Drv pandocommand_loader`. The old `make drvr-run-bfs_multi_sw` is no longer the canonical entry point.

  ---

  ## Running

  The simplest path is to copy and adapt one of the existing `*.sbatch` files (e.g. `bfs_csr_shared_queue.sbatch`)  they wire up build + run + sweep loop +
  summarization in one file.

  If you want to run interactively, the runtime apptainer invocation looks like:

  ```bash
  SIF=/work2/10238/vineeth_architect/stampede3/drv_latest.sif
  RAMULATOR_SRC=/work2/10238/vineeth_architect/stampede3/drv_copy/ramulator-build
  RAMULATOR_LIB=$RAMULATOR_SRC/libramulator.so
  RAMULATOR_CONFIGS=$RAMULATOR_SRC/configs
  MEMH_LIB=/work2/10238/vineeth_architect/stampede3/drv-stack/sst-elements/lib/sst-elements-library/libmemHierarchy.so

  apptainer exec --cleanenv \
    --env XALT_EXECUTABLE_TRACKING=no \
    --env OMPI_MCA_mtl=^psm2 \
    --bind "$PWD:/work" \
    --bind "$RAMULATOR_LIB:/install/lib/libramulator.so" \
    --bind "$RAMULATOR_CONFIGS:/work/ramulator-configs" \
    --bind "$MEMH_LIB:/install/lib/sst-elements-library/libmemHierarchy.so" \
    "$SIF" \
    bash
  ```

  Run-step additions vs. the older README:
  - `--env OMPI_MCA_mtl=^psm2` — disables PSM2 MTL (avoids MPI/OFI noise on Stampede3 fabric).
  - `--bind $RAMULATOR_LIB → /install/lib/libramulator.so` — same override as build.
  - `--bind $RAMULATOR_CONFIGS → /work/ramulator-configs` — the model passes `--dram-backend-config-sliced=/work/ramulator-configs/HBM-pando-16ch.cfg`, so this mount
   is required at runtime.
  - `--bind $MEMH_LIB → /install/lib/sst-elements-library/libmemHierarchy.so` — overrides the container's `libmemHierarchy.so` with the DRV_CACHE_ALU build from Step
   3.

  ### Inside the container - invoking SST directly

  ```bash
  PYTHONPATH=/work/py::/work/model \
    /install/bin/sst -n 1 /work/model/drvr.py -- \
      --with-command-processor=/work/build_stampede/pandocommand/libpandocommand_loader.so \
      --num-pxn=1 --pxn-pods=1 --pod-cores-x=8 --pod-cores-y=8 --core-threads=16 \
      --pod-l2sp-banks=4 --pod-l2sp-interleave=64 \
      --pxn-dram-banks=1 --pxn-dram-cache-size=$((512*1024)) --pxn-dram-cache-slices=4 \
      --dram-backend-config-sliced=/work/ramulator-configs/HBM-pando-16ch.cfg \
      --pxn-dram-cache-alu=0 \
      /work/build_stampede/rv64/drvr/drvr_bfs_csr_shared_queue_baseline \
      --V 1024 --D 16
  ```

  ---

  ## Why the libramulator / libmemHierarchy separation matters

  - Inside the container, `/install/lib/libramulator.so` is the Dockerfile's stock build.
  - Outside, `drv_copy/ramulator-build/libramulator.so` is the same source rebuilt against the host glibc with the SST patches — and it's the one your rebuilt
  `libmemHierarchy.so` was linked against.
  - If you run with the in-container ramulator, the rebuilt `libmemHierarchy.so` may load a `libramulator.so` whose symbols don't match hence the bind-mount
  override on every run.




