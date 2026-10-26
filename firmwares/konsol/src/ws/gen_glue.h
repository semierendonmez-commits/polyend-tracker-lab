// Generator parameter descriptors for every clip player (gen_glue.cpp).
#pragma once

#include "gen/gen.h"
#include "ws/model.h"

namespace lab {
namespace konsol {

// Parameter i of player `kind` (name == nullptr: unused). v = the clip's values
// (BBCut's A / B depend on its procedure).
const ot::gen::ParamDesc& genParam(int kind, int i, const int16_t* v);
// Steps of an usul clip: the cycle in 1/8 units x 2 x repeats.
int usulSteps(int usul, int repeats);

}  // namespace konsol
}  // namespace lab
