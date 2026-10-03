// FREQ_TRACE / classic amp search live here. The geometric amp-grid builder
// defines FREQ_TRACE_AMP_GEOM_GRID — if _shared is a stale symlink to an old
// DCO-SHARED-LIBRARIES tree, fail the build instead of silently running the
// freq-ladder path that wrote DIV_COUNTER+1 endpoints.
#include "_shared/autotune_search_impl.h"
#ifndef FREQ_TRACE_AMP_GEOM_GRID
#error "Stale _shared: FREQ_TRACE amp-geom grid missing. Use this branch's vendored _shared (not an old DCO-SHARED-LIBRARIES symlink)."
#endif
