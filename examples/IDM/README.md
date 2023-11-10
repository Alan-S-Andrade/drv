SPDX-License-Identifier: MIT

The application assumes a binary image of CSR is loaded to the last bank of DRAM
to start simulation, cd to `$PROJ_ROOT/tests`, then `sst PANDOHammerDrvX-IDM.py -- ./../examples/IDM/app-main.so`
When a thread finishes, it outputs some statistics like below
```
 4 done; work: 387, sampled edges: 37445, sampled vertices: 10141
avg sampled edges: 96.76, avg sampled vertices: 26.20
V local: 7117, V remote: 3024
E local: 7587, E remote: 6065
```