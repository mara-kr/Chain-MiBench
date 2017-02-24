#ifndef EDB_H
#define EDB_H

// This header is a wrapper that conditionally either includes EDB headers or
// stubs its public API. This wrapper exists to be able to build with and
// without EDB without having to edit the application source. Note that
// obviously, this functionality cannot be encapsulated into libdino, because
// the point of building without EDB is to not depend on libdino. But, we keep
// this header in the EDB repo, and let each app copy it as needed.

#ifdef CONFIG_EDB

#include <libedb/edb.h>

#else // !EDB

#define EDB_INIT()

#define WATCHPOINT(...)

#define EXTERNAL_BREAKPOINT(...)
#define INTERNAL_BREAKPOINT(...)
#define PASSIVE_BREAKPOINT(...)

#define ENERGY_GUARD_BEGIN()
#define ENERGY_GUARD_END()

#endif // !EDB

#endif // EDB_H
