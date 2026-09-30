#ifndef WISP_INTERNAL_H
#define WISP_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef __cplusplus
#define WISP_SASSERT(c, m) static_assert(c, m)
#else
#define WISP_SASSERT(c, m) _Static_assert(c, m)
#endif

#ifdef __cplusplus
#define WISP_ALIGNAS(n) alignas(n)
#define WISP_ALIGNOF(t) alignof(t)
#else
#define WISP_ALIGNAS(n) _Alignas(n)
#define WISP_ALIGNOF(t) _Alignof(t)
#endif

#define WISP_CONTAINER_OF(ptr, type, member)                                           \
  ({                                                                                   \
    const typeof(((type *)0)->member) *__mptr = (ptr);                                 \
    (type *)((char *)__mptr - offsetof(type, member));                                 \
  })

#define WISP_CAT_(a, b) a##b
#define WISP_CAT(a, b) WISP_CAT_(a, b)

#define WISP_CACHELINE 64
/* Reserve an **entire cache line** for this field. Pads with
 * WISP_CACHELINE - sizeof(type) bytes in anon. union.
 *
 * Use very sparingly.
 */
#define WISP_CACHELINE_FIELD(type, name)                                               \
  union {                                                                              \
    WISP_ALIGNAS(WISP_CACHELINE) type name;                                            \
    unsigned char WISP_CAT(pad_, __LINE__)[WISP_CACHELINE];                            \
  }

/*
 * Marker for what should be a `atomic_*|_Atomic` type but isn't for cross-language ABI
 * stability and reinterpretation.
 *
 * Plain, fixed-width, non "_Atomic/atomic_*" types. Accessed only through
 * wisp__a{load,store,fetch_add}_*(), never through <stdatomic.h>'s
 * atomic_load()/atomic_store()/etc, which require an _Atomic-qualified (C) or
 * std::atomic<T> (C++) operand and will not accept these.
 */
#define WISP_ATOMIC

/*
 * wisp__atomic_{load,store,fetch_add}_{i32,u32,bool} - the single, unified atomic
 * primitive used throughout, for cross-language ABI stability.
 *
 * <stdatomic.h>'s atomic_load()/atomic_store()/atomic_fetch_add() require a pointer to
 * an _Atomic-qualified type in C and a pointer to std::atomic<T> in C++. Since every
 * field these helpers touch is deliberately a plain, fixed-width, non-_Atomic type (a
 * plain int32_t has one obvious binary layout; an _Atomic int / std::atomic<int> does
 * not, and differs across compilers/languages), <stdatomic.h>'s macros cannot be used
 * on them at all.
 *
 * Instead, every atomic access goes through the GNU/Clang-common __atomic_* builtins,
 * which operate on any object of a matching size/alignment and are supported
 * identically by both compilers in both C and C++.
 *
 * Note the converse also holds and this must be applied uniformly rather than
 * mixed with <stdatomic.h>: __atomic_* builtins reject genuinely
 * _Atomic-qualified/std::atomic<T> operands under Clang (both C and C++) and under GCC
 * in C++ mode. Do not mix them together, and just use the WISP_ATOMIC flavor.
 */

static inline int32_t wisp__atomic_load_i32(const int32_t *p) {
  return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void wisp__atomic_store_i32(int32_t *p, int32_t v) {
  __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}
static inline uint32_t wisp__atomic_load_u32(const uint32_t *p) {
  return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void wisp__atomic_store_u32(uint32_t *p, uint32_t v) {
  __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}
static inline uint32_t wisp__atomic_fetch_add_u32(uint32_t *p, uint32_t v) {
  return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}
static inline uint64_t wisp__atomic_load_u64(const uint64_t *p) {
  return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void wisp__atomic_store_u64(uint64_t *p, uint64_t v) {
  __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}
static inline uint64_t wisp__atomic_fetch_add_u64(uint64_t *p, uint64_t v) {
  return __atomic_fetch_add(p, v, __ATOMIC_SEQ_CST);
}
static inline bool wisp__atomic_load_bool(const bool *p) {
  return __atomic_load_n(p, __ATOMIC_SEQ_CST);
}
static inline void wisp__atomic_store_bool(bool *p, bool v) {
  __atomic_store_n(p, v, __ATOMIC_SEQ_CST);
}

#ifndef __cplusplus

#define wisp_atomic_load(p)                                                            \
  _Generic((p),                                                                        \
      const int32_t *: wisp__atomic_load_i32,                                          \
      int32_t *: wisp__atomic_load_i32,                                                \
      const uint32_t *: wisp__atomic_load_u32,                                         \
      uint32_t *: wisp__atomic_load_u32,                                               \
      const uint64_t *: wisp__atomic_load_u64,                                         \
      uint64_t *: wisp__atomic_load_u64,                                               \
      const bool *: wisp__atomic_load_bool,                                            \
      bool *: wisp__atomic_load_bool)(p)

#define wisp_atomic_store(p, v)                                                        \
  _Generic((p),                                                                        \
      int32_t *: wisp__atomic_store_i32,                                               \
      uint32_t *: wisp__atomic_store_u32,                                              \
      uint64_t *: wisp__atomic_store_u64,                                              \
      bool *: wisp__atomic_store_bool)((p), (v))

#define wisp_atomic_fetch_add(p, v)                                                    \
  _Generic((p),                                                                        \
      uint32_t *: wisp__atomic_fetch_add_u32,                                          \
      uint64_t *: wisp__atomic_fetch_add_u64)((p), (v))

/* Have to use overload resolution for C++, so below extern "C" closer maps directly
 * to the opening extern "C" of header, and is reopened at end of block
 */
#else
} /* extern "C" */

static inline int32_t wisp_atomic_load(const int32_t *p) {
  return wisp__atomic_load_i32(p);
}
static inline uint32_t wisp_atomic_load(const uint32_t *p) {
  return wisp__atomic_load_u32(p);
}
static inline uint64_t wisp_atomic_load(const uint64_t *p) {
  return wisp__atomic_load_u64(p);
}
static inline bool wisp_atomic_load(const bool *p) { return wisp__atomic_load_bool(p); }

static inline void wisp_atomic_store(int32_t *p, int32_t v) {
  wisp__atomic_store_i32(p, v);
}
static inline void wisp_atomic_store(uint32_t *p, uint32_t v) {
  wisp__atomic_store_u32(p, v);
}
static inline void wisp_atomic_store(uint64_t *p, uint64_t v) {
  wisp__atomic_store_u64(p, v);
}
static inline void wisp_atomic_store(bool *p, bool v) { wisp__atomic_store_bool(p, v); }

static inline uint32_t wisp_atomic_fetch_add(uint32_t *p, uint32_t v) {
  return wisp__atomic_fetch_add_u32(p, v);
}
static inline uint64_t wisp_atomic_fetch_add(uint64_t *p, uint64_t v) {
  return wisp__atomic_fetch_add_u64(p, v);
}

extern "C" {
#endif /* __cplusplus */

static inline uint64_t wisp_ts_to_ns(const struct timespec *ts) {
  return ((uint64_t)ts->tv_sec * 1000000000ULL) + ts->tv_nsec;
}

static inline struct timespec wisp_ns_to_ts(uint64_t ns) {
  struct timespec ts;
  ts.tv_sec = ns / 1000000000ULL;
  ts.tv_nsec = ns % 1000000000ULL;
  return ts;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* WISP_INTERNAL_H */