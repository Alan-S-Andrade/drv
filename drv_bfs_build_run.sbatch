#!/bin/bash
#SBATCH -J drv_bfs
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006        # <-- REQUIRED: replace with your Stampede3 allocation
#SBATCH -p skx                    # skx is common default; use spr if you need it
#SBATCH -N 1
#SBATCH -n 1
#SBATCH -c 32
#SBATCH -t 01:00:00

set -euo pipefail

cd "$SLURM_SUBMIT_DIR"
mkdir -p logs

module purge
module load tacc-apptainer/1.4.1

SIF="$WORK/drv.sif"

apptainer exec --cleanenv \
  --env XALT_EXECUTABLE_TRACKING=no \
  --bind "$SLURM_SUBMIT_DIR:/work" \
  "$SIF" \
  bash -lc '
    set -euo pipefail
    cd /work

    export RISCV_HOME=/install
    export PATH=/install/bin:$PATH

    mkdir -p build
    cd build

    cmake .. \
      -DSST_CORE_PREFIX=/install \
      -DSST_ELEMENTS_PREFIX=/install \
      -DCMAKE_INSTALL_PREFIX=/work/.local

    make -j'"${SLURM_CPUS_PER_TASK}"' drvr-run-bfs_multi_sw

    exe=$(find . -maxdepth 4 -type f -name drvr-run-dense_gemm | head -n 1 || true)
    if [[ -z "$exe" ]]; then
      echo "ERROR: drvr-run-bfs_multi_sw not found after build."
      find . -maxdepth 4 -type f -executable | head -n 80
      exit 2
    fi

    echo "Running: $exe"
    "$exe"
  '

