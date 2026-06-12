#ifndef MG_PLATFORM_LITE_H
#define MG_PLATFORM_LITE_H
// Force-included before any TU that pulls in mongoose.h on the LibreTiny build.
// The actual platform definitions live in lib/MongooseLite/platform_custom.h
// (which mongoose.h finds via -I lib/MongooseLite as <platform_custom.h>).
// Including it here ensures CS_PLATFORM=CS_P_CUSTOM and LWIP_COMPAT_SOCKETS=1
// are defined before any include order surprises.
#include "platform_custom.h"
#endif // MG_PLATFORM_LITE_H
