// Global allocation accounting for per-case peak heap (C++ allocations,
// which include the statically linked ckey core). Single-threaded worker.
#include <cstddef>
#include <cstdlib>
#include <malloc.h>
#include <new>

namespace ckt_alloc {
size_t current = 0, peak = 0;
}

static void *track(void *p) {
    if (!p) throw std::bad_alloc();
    ckt_alloc::current += malloc_usable_size(p);
    if (ckt_alloc::current > ckt_alloc::peak) ckt_alloc::peak = ckt_alloc::current;
    return p;
}
static void untrack(void *p) {
    if (!p) return;
    ckt_alloc::current -= malloc_usable_size(p);
    std::free(p);
}

void *operator new(size_t n) { return track(std::malloc(n ? n : 1)); }
void *operator new[](size_t n) { return track(std::malloc(n ? n : 1)); }
void *operator new(size_t n, std::align_val_t a) { return track(std::aligned_alloc(static_cast<size_t>(a), (n + static_cast<size_t>(a) - 1) / static_cast<size_t>(a) * static_cast<size_t>(a))); }
void *operator new[](size_t n, std::align_val_t a) { return operator new(n, a); }
void *operator new(size_t n, const std::nothrow_t &) noexcept { void *p = std::malloc(n ? n : 1); if (p) { ckt_alloc::current += malloc_usable_size(p); if (ckt_alloc::current > ckt_alloc::peak) ckt_alloc::peak = ckt_alloc::current; } return p; }
void *operator new[](size_t n, const std::nothrow_t &t) noexcept { return operator new(n, t); }
void operator delete(void *p) noexcept { untrack(p); }
void operator delete[](void *p) noexcept { untrack(p); }
void operator delete(void *p, size_t) noexcept { untrack(p); }
void operator delete[](void *p, size_t) noexcept { untrack(p); }
void operator delete(void *p, std::align_val_t) noexcept { untrack(p); }
void operator delete[](void *p, std::align_val_t) noexcept { untrack(p); }
void operator delete(void *p, size_t, std::align_val_t) noexcept { untrack(p); }
void operator delete[](void *p, size_t, std::align_val_t) noexcept { untrack(p); }
void operator delete(void *p, const std::nothrow_t &) noexcept { untrack(p); }
void operator delete[](void *p, const std::nothrow_t &) noexcept { untrack(p); }
