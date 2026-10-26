# DATAMATICS -- third-party code and borrowed designs

No third-party source code is compiled into `src/ws/`. Everything there was
written for this firmware.

| What | From | Licence | How it is used |
|---|---|---|---|
| VIEW modes TRANSIT, BARCODE, GRID, SPATIAL and the meta block (file, type, entropy, hash, offset) | the user's own IKEDAT plugin (`/home/claude/ikedat`, `Source/ui/Visual.h`) | the user's project (written by Claude for the user; its NOTICE: "written for this port") | the visual designs (rail + sorting fan, bit-run rows with digit strips, byte-pair network with a crosshair, byte-triplet point cloud in a cube) were redrawn from scratch for the FT81x display list; no code copied |
| Section and throw ideas (scan, densify, silence, freeze, stutter) | the user's PINDATA / DATASCREEN / SETBOOK / BLACKBOX designs | the user's projects | ideas only |
| `src/lab/` (the lab Shell, LABKIT v2) | polyend-tracker-lab | same as the repository | synced with `labkit/tools/sync_labkit.sh`; one local fix listed in the report (MAP column budget) |

GPL code is approved for this repository by its owner, but none is used here.
