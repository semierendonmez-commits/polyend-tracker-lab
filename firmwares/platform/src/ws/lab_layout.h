// PLATFORM -- a fingerprint of the lab.h object layouts that "station" apps
// (a lab::Workstation built as a native app, sdk/station/) share with the
// host: they use the host's Shell / Canvas / IStorage objects directly. The
// host puts kLabLayout in PlatHost::labLayout; the station glue computes the
// same expression from its copy of the headers and refuses to start on a
// mismatch (an app built against other lab.h sources).
#pragma once

#include <cstddef>
#include <cstdint>

#include "lab/lab.h"

namespace plat {

constexpr uint32_t layoutMix(uint32_t h, uint32_t v) { return (h ^ v) * 16777619u; }

constexpr uint32_t kLabLayout = layoutMix(
    layoutMix(layoutMix(layoutMix(layoutMix(layoutMix(layoutMix(layoutMix(layoutMix(2166136261u, uint32_t(sizeof(lab::Shell))),
                                                                          uint32_t(sizeof(lab::Canvas))),
                                                                uint32_t(sizeof(lab::Context))),
                                                      uint32_t(sizeof(lab::Event))),
                                            uint32_t(sizeof(lab::Transport))),
                                  uint32_t(sizeof(lab::Theme))),
                        uint32_t(sizeof(lab::SettingRow))),
              uint32_t(sizeof(lab::Arena))),
    uint32_t(lab::Canvas::kBudgetWords) + uint32_t(lab::kBlock) * 65536u);

}  // namespace plat
