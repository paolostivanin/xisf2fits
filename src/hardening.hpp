#pragma once
// Define this before any system header, but honor stronger distro/toolchain
// settings. __OPTIMIZE__ also covers RelWithDebInfo and custom optimized builds.
#if defined(__OPTIMIZE__) && !defined(_FORTIFY_SOURCE)
#define _FORTIFY_SOURCE 2
#endif
