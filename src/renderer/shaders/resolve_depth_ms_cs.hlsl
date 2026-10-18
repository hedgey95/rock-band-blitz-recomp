// Depth resolve from a multisampled depth target: sample 0, as Xenos resolves
// depth without averaging. See resolve_depth_cs.hlsl.
#define MULTISAMPLED
#include "resolve_depth_cs.hlsl"
