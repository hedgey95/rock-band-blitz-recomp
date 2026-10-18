// One hook per XDK D3D function; see d3d_hooks.h.

#include "d3d_hooks.h"

#include <vector>

#include <rex/hook.h>

namespace blitz::d3d {
namespace detail {
std::vector<Listener*>& Listeners() {
  static std::vector<Listener*> listeners;
  return listeners;
}
}  // namespace detail

void AddListener(Listener* listener) { detail::Listeners().push_back(listener); }

}  // namespace blitz::d3d

#define D3D_HOOK(sym, addr)                                                                   \
  REX_EXTERN(__imp__##sym);                                                                   \
  REX_HOOK_RAW(sym) {                                                                         \
    const auto& listeners = blitz::d3d::detail::Listeners();                                  \
    for (auto* listener : listeners) listener->Before(0x##addr, ctx, base);                   \
    __imp__##sym(ctx, base);                                                                  \
    for (auto it = listeners.rbegin(); it != listeners.rend(); ++it)                          \
      (*it)->After(0x##addr, ctx, base);                                                      \
  }

// Named D3D functions (config/functions.toml).
D3D_HOOK(D3DDevice_BeginVertices, 8251A7D8)
D3D_HOOK(D3DDevice_BeginIndexedVertices, 8251ACE0)
D3D_HOOK(D3DDevice_DrawVertices, 8251B220)
D3D_HOOK(D3DDevice_DrawIndexedVertices, 8251B638)
D3D_HOOK(D3DDevice_EndVertices, 8251A7C8)
D3D_HOOK(D3DDevice_DrawVerticesUP, 8251AC98)
D3D_HOOK(D3D_FlushRegisters, 82521420)
D3D_HOOK(D3D_FlushClipPlanes, 825214E0)
D3D_HOOK(D3D_FlushFetchConstants, 82521660)
D3D_HOOK(D3D_FlushFloatConstants, 825217B8)
D3D_HOOK(D3D_FlushShaders, 82522550)
D3D_HOOK(D3D_CreateShader, 82516108)
D3D_HOOK(D3D_RegisterVertexShader, 82516010)
D3D_HOOK(D3D_RegisterPixelShader, 82514420)
D3D_HOOK(D3DDevice_Swap, 8251D580)

// The other D3D-cluster functions that write the command buffer (they store
// device+48), plus two outside the cluster that build draw headers.
D3D_HOOK(sub_8250E1A0, 8250E1A0)
D3D_HOOK(sub_8250E2F8, 8250E2F8)
D3D_HOOK(sub_825124E0, 825124E0)
D3D_HOOK(sub_82515D98, 82515D98)
D3D_HOOK(sub_82519490, 82519490)
D3D_HOOK(sub_82527410, 82527410)
D3D_HOOK(sub_8252E6F8, 8252E6F8)
D3D_HOOK(sub_82506568, 82506568)
D3D_HOOK(sub_82507C40, 82507C40)
D3D_HOOK(sub_82508428, 82508428)
D3D_HOOK(sub_82508938, 82508938)
D3D_HOOK(sub_8250B408, 8250B408)
D3D_HOOK(D3DDevice_Resolve, 8250C938)
D3D_HOOK(sub_8250DD78, 8250DD78)
D3D_HOOK(sub_8250E680, 8250E680)
D3D_HOOK(sub_8250E778, 8250E778)
D3D_HOOK(sub_8250ED08, 8250ED08)
D3D_HOOK(sub_8250EE78, 8250EE78)
D3D_HOOK(sub_8250F4B8, 8250F4B8)
D3D_HOOK(sub_8250F528, 8250F528)
D3D_HOOK(sub_8250F6B0, 8250F6B0)
D3D_HOOK(sub_8250FF78, 8250FF78)
D3D_HOOK(sub_825105E8, 825105E8)
D3D_HOOK(sub_82512028, 82512028)
D3D_HOOK(sub_82512730, 82512730)
D3D_HOOK(sub_82514890, 82514890)
D3D_HOOK(sub_82514A00, 82514A00)
D3D_HOOK(sub_825199B0, 825199B0)
D3D_HOOK(D3DDevice_ClearF, 82519F50)
D3D_HOOK(sub_8251BBD0, 8251BBD0)
D3D_HOOK(D3DDevice_BeginTiling, 8251BD40)
D3D_HOOK(sub_8251C910, 8251C910)
D3D_HOOK(sub_8251C9B8, 8251C9B8)
D3D_HOOK(sub_8251D0C0, 8251D0C0)
D3D_HOOK(sub_8251D2A0, 8251D2A0)
D3D_HOOK(sub_8251D358, 8251D358)
D3D_HOOK(sub_8251DCE8, 8251DCE8)
D3D_HOOK(sub_8251DD48, 8251DD48)
D3D_HOOK(sub_8251E690, 8251E690)
D3D_HOOK(sub_8251F430, 8251F430)
D3D_HOOK(sub_8251F940, 8251F940)
D3D_HOOK(sub_8251FA20, 8251FA20)
D3D_HOOK(sub_8251FA98, 8251FA98)
D3D_HOOK(sub_8251FAF0, 8251FAF0)
D3D_HOOK(sub_82520B10, 82520B10)
D3D_HOOK(sub_82520C00, 82520C00)
D3D_HOOK(sub_82520DE0, 82520DE0)
D3D_HOOK(sub_82520E98, 82520E98)
D3D_HOOK(sub_82521090, 82521090)
D3D_HOOK(sub_82522208, 82522208)
D3D_HOOK(sub_82522360, 82522360)
D3D_HOOK(sub_82522410, 82522410)
D3D_HOOK(sub_82522F78, 82522F78)
D3D_HOOK(sub_82524AB0, 82524AB0)
D3D_HOOK(sub_82527138, 82527138)
D3D_HOOK(sub_82527190, 82527190)
D3D_HOOK(sub_82527210, 82527210)
D3D_HOOK(sub_825272E8, 825272E8)
D3D_HOOK(sub_825273A8, 825273A8)
D3D_HOOK(sub_825274B0, 825274B0)
D3D_HOOK(sub_825280F0, 825280F0)
D3D_HOOK(sub_82528528, 82528528)
D3D_HOOK(sub_82528568, 82528568)
D3D_HOOK(sub_82528D20, 82528D20)
D3D_HOOK(sub_82529C20, 82529C20)
D3D_HOOK(sub_82529EC8, 82529EC8)
D3D_HOOK(sub_82529F58, 82529F58)
D3D_HOOK(sub_8252A3F8, 8252A3F8)
D3D_HOOK(sub_8252BE10, 8252BE10)
D3D_HOOK(sub_8252C088, 8252C088)
D3D_HOOK(sub_8252C3F8, 8252C3F8)
D3D_HOOK(sub_82535170, 82535170)
D3D_HOOK(sub_82483590, 82483590)
D3D_HOOK(sub_82597670, 82597670)

// Command buffer kick-off: past the limit at device+56, D3D submits the segment
// and continues in the next one.
D3D_HOOK(sub_8251F770, 8251F770)
