// DATAMATICS -- the workstation factory (dm_ws.h).
#include "ws/dm_ws.h"

namespace lab {
Workstation* createWorkstation(Arena& arena) { return arena.make<dm::Datamatics>(); }
}  // namespace lab
