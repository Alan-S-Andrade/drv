#!/bin/bash
#SBATCH -J drv_bfs
#SBATCH -o logs/%x_%j.out
#SBATCH -e logs/%x_%j.err

#SBATCH -A CCR22006
#SBATCH -p icx              # Use dev queue for speed
#SBATCH -N 1
#SBATCH -n 1
#SBATCH -c 32
#SBATCH -t 06:00:00

# 1. Use -eo pipefail (Removed 'u')
set -eo pipefail

cd "$SLURM_SUBMIT_DIR"
mkdir -p logs

module purge

# 2. NUCLEAR OPTION: Explicitly turn off unbound variable checking
set +u  

module load tacc-apptainer/1.4.1

SIF="$WORK/drv.sif"

if [[ ! -f "$SIF" ]]; then
    echo "ERROR: Container image not found at $SIF"
    exit 1
fi

apptainer exec --cleanenv \
  --env XALT_EXECUTABLE_TRACKING=no \
  --bind "$SLURM_SUBMIT_DIR:/work" \
  "$SIF" \
  bash -lc '
    # Re-enable safety inside the container if you want, or leave it off
    set -e 
    
    export RISCV_HOME=/install
    export PATH=/install/bin:$PATH
    
    cd build_stampede

    cmake .. \
      -DSST_CORE_PREFIX=/install \
      -DSST_ELEMENTS_PREFIX=/install \
      -DCMAKE_INSTALL_PREFIX=/work/.local

    make -j'"${SLURM_CPUS_PER_TASK}"' drvr-run-bfs_multi_sw

    echo "Simulation complete"
  '
