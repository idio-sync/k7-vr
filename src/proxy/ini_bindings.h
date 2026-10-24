#pragma once
// killer7.ini text for the VR profile (vr_profile.cpp): the keyboard bindings the VR controllers
// send (input.cpp) and windowed mode, everything else as it was. Pure, so it's unit-tested.
#include <string>

namespace k7 {

std::string vr_ini_apply(const std::string& ini);

}  // namespace k7
