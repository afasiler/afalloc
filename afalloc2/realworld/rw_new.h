/* Replaces the global operator new/delete of a C++ program:
 *   -DUSE_AFALLOC    every new/delete goes to afalloc/afree
 *   -DCOUNT_ALLOCS   count allocations and bytes (the system allocator still serves them) and print at exit
 * With neither, the program uses the system allocator, so the same source runs unchanged. */
#pragma once
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <new>

#if defined(USE_AFALLOC)
#include "../afalloc.h"
void *operator new(std::size_t n) { void *p = afalloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void *operator new[](std::size_t n) { return operator new(n); }
void *operator new(std::size_t n, const std::nothrow_t &) noexcept { return afalloc(n ? n : 1); }
void *operator new[](std::size_t n, const std::nothrow_t &) noexcept { return afalloc(n ? n : 1); }
void operator delete(void *p) noexcept { afree(p); }
void operator delete[](void *p) noexcept { afree(p); }
void operator delete(void *p, std::size_t) noexcept { afree(p); }
void operator delete[](void *p, std::size_t) noexcept { afree(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { afree(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { afree(p); }
void *operator new(std::size_t n, std::align_val_t a) { void *p = afaligned_alloc((std::size_t)a, n); if (!p) throw std::bad_alloc(); return p; }
void *operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void operator delete(void *p, std::align_val_t) noexcept { afree(p); }
void operator delete[](void *p, std::align_val_t) noexcept { afree(p); }
void operator delete(void *p, std::size_t, std::align_val_t) noexcept { afree(p); }
void operator delete[](void *p, std::size_t, std::align_val_t) noexcept { afree(p); }
#elif defined(COUNT_ALLOCS)
static unsigned long g_calls, g_bytes;
void *operator new(std::size_t n) { g_calls++; g_bytes += n; void *p = std::malloc(n ? n : 1); if (!p) throw std::bad_alloc(); return p; }
void *operator new[](std::size_t n) { return operator new(n); }
void operator delete(void *p) noexcept { std::free(p); }
void operator delete[](void *p) noexcept { std::free(p); }
void operator delete(void *p, std::size_t) noexcept { std::free(p); }
void operator delete[](void *p, std::size_t) noexcept { std::free(p); }
struct AllocReporter { ~AllocReporter() { std::printf("alloc_calls,%lu\nalloc_bytes_MB,%.1f\n", g_calls, (double)g_bytes / 1048576.0); } } g_alloc_reporter;
#endif

#include <chrono>
static inline double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
static inline void metric(const char *name, double v) { std::printf("%s,%.3f\n", name, v); std::fflush(stdout); }
