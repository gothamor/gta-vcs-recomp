// The build machine's stand-in for FreeBSD's <pthread_np.h>: platform_ps5.cpp compiled for the controller test (ps5/tests/padtest.cpp)
#pragma once
#include <pthread.h>
static inline void pthread_set_name_np(pthread_t t, const char *n) { pthread_setname_np(t, n); }
