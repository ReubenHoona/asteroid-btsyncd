/*
 * Out-of-class definitions of static const members that the daemon's sources odr-use (bound to
 * the const references of qBound): an optimised build (the image's) folds them away, an -O0 build
 * (Debug, coverage) needs a definition to link. C++17 makes only constexpr members inline.
 */
#include "callaudio.h"

const int CallAudio::VolumeMax;
