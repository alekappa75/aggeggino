/*
 * atomic_stub.c — provides __atomic_is_lock_free for the xtensa-esp-elf
 * toolchain, which ships no libatomic.
 *
 * Ableton Link calls std::atomic<T>::is_lock_free() on types larger than the
 * native word; GCC emits a call to __atomic_is_lock_free(size, ptr) that would
 * otherwise remain unresolved at link time.
 *
 * Semantics, per the GCC specification: return true if an atomic access to an
 * object of 'size' bytes at the given alignment is lock-free. On the dual-core
 * ESP32-S3, aligned accesses up to the word size (4 bytes) are lock-free;
 * larger ones are not. The answer below is conservative and correct.
 *
 * NOTE: this does NOT change Link's runtime behaviour — it still uses the
 * __atomic_load/store builtins GCC provides. It only resolves the is_lock_free
 * query symbol.
 */
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

bool __atomic_is_lock_free(size_t size, const volatile void *ptr)
{
    /* Lock-free when the size is a power of two no larger than the native word
     * (4 bytes) and the object is aligned to its own size. */
    if (size == 0 || size > 4) return false;
    if (size & (size - 1)) return false;                 /* not a power of two */
    if (ptr && (((uintptr_t)ptr) & (size - 1))) return false; /* misaligned */
    return true;
}
