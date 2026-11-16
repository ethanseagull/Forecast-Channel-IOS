// NW4R g3d pieces the decomp does not define because CodeWarrior stripped them
// from the DOL (nothing called them there), but Clang's vtables reference.
#include <nw4r/g3d.h>

#include <port_log.h>

namespace nw4r {
namespace g3d {

// Unbinds the animation from its model. The base class keeps no binding
// state of its own beyond the flag.
void AnmObj::Release() {
    SetAnmFlag(FLAG_ANM_BOUND, false);
}

} // namespace g3d
} // namespace nw4r
