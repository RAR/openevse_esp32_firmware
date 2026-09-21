#pragma once
// SPIKE: force-included into every TU of the S3 Matter env (see platformio.ini).
//
// GCC 14 + gnu++2b: std::optional<T> == std::optional<T> needs a same-type
// operator==, and CHIP's ClosureControl structs only define one against their
// base type. Espressif ships this exact fix as libraries/Matter/src/
// matter_closure_patch.h and -includes it into esp_matter from the newer
// Arduino cores' CMake; 3.3.11's hybrid CMake does not, so it is inlined here.
// Only TUs that can see CHIP's headers get it; IDF components built with a
// narrower include set skip it.
#if defined(__cplusplus) && __has_include("app/clusters/closure-control-server/closure-control-cluster-objects.h")
#if __has_include("matter_closure_patch.h")
// TUs that can see the Arduino Matter library (and will include Matter.h,
// which pulls the same header): use Espressif's copy so nothing is defined twice.
#include "matter_closure_patch.h"
#else
#include "app/clusters/closure-control-server/closure-control-cluster-objects.h"

namespace chip { namespace app { namespace Clusters { namespace ClosureControl {

inline bool operator==(const GenericOverallCurrentState &a, const GenericOverallCurrentState &b) {
  return a.position == b.position && a.latch == b.latch && a.speed == b.speed && a.secureState == b.secureState;
}

inline bool operator==(const GenericOverallTargetState &a, const GenericOverallTargetState &b) {
  return a.position == b.position && a.latch == b.latch && a.speed == b.speed;
}

}}}}  // namespace chip::app::Clusters::ClosureControl
#endif  // __has_include("matter_closure_patch.h")
#endif
