// Synthetic memory only; this test does not load a game or install a handler.
#include "linux_guest_ram.h"

#if defined(__linux__) && defined(__x86_64__)
#include "core/cpu.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <algorithm>
#include <cassert>
#include <csignal>
#include <cerrno>
#include <cstdio>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>

namespace {
template <typename T>
T fastmem_load(const uint8_t* pointer) {
  if constexpr (sizeof(T) == 8) return dolrecomp_fm_ld64(pointer);
  else if constexpr (sizeof(T) == 4) return dolrecomp_fm_ld32(pointer);
  else if constexpr (sizeof(T) == 2) return T(dolrecomp_fm_ld16(pointer));
  else return T(dolrecomp_fm_ld8(pointer));
}
template <typename T>
void fastmem_store(uint8_t* pointer, T value) {
  if constexpr (sizeof(T) == 8) dolrecomp_fm_st64(pointer, value);
  else if constexpr (sizeof(T) == 4) dolrecomp_fm_st32(pointer, value);
  else if constexpr (sizeof(T) == 2) dolrecomp_fm_st16(pointer, value);
  else dolrecomp_fm_st8(pointer, value);
}
template <typename T>
__attribute__((noinline)) void check_fastmem_alias(uint8_t* view, T first, T second) {
  auto* physical = view + 0x2000;
  auto* cached = view + 0x80002000ull;
  auto* uncached = view + 0xC0002000ull;
  fastmem_store(cached, first);
  const T before = fastmem_load<T>(cached);
  fastmem_store(uncached, second);
  const T after = fastmem_load<T>(cached);
  assert(before == first && after == second);
  assert(fastmem_load<T>(physical) == second);
  fastmem_store(physical, first);
  assert(fastmem_load<T>(uncached) == first);
}
__attribute__((noinline)) void check_fastmem_mixed_access(uint8_t* view) {
  auto* ordinary = reinterpret_cast<const uint32_t*>(view + 0x80003000ull);
  auto* uncached = view + 0xC0003000ull;
  dolrecomp_fm_st32(uncached, 0x13579BDFu);
  const uint32_t before = *ordinary;
  dolrecomp_fm_st32(uncached, 0x2468ACE0u);
  const uint32_t after = *ordinary;
  assert(before == 0x13579BDFu && after == 0x2468ACE0u);
}

void expect_guard_fault(uint8_t* address) {
  const pid_t child = ::fork();
  assert(child >= 0);
  if (!child) {
    const rlimit no_core{0, 0};
    ::setrlimit(RLIMIT_CORE, &no_core);
    // Test the mapping's protection without changing process signal handlers.
    *static_cast<volatile uint8_t*>(address) = 1;
    ::_exit(1);
  }
  int status = 0;
  assert(::waitpid(child, &status, 0) == child);
  assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGSEGV);
}
}

int main() {
  const char* previous = std::getenv("STANDALONE_FASTMEM");
  const bool had_previous = previous != nullptr;
  const std::string saved = previous ? previous : "";
  ::setenv("STANDALONE_FASTMEM", "1", 1);
  uint8_t* released_view = nullptr;
  {
    LinuxGuestRam ram;
    assert(ram.size() == 24u * 1024u * 1024u);
    auto* view = ram.fastmem_view();
    released_view = view;
    assert(view);  // CI must exercise real alias mappings, not silently skip.
    assert(ram.data() == view);
    assert(std::all_of(ram.data(), ram.data() + ram.size(), [](uint8_t v) { return v == 0; }));
    // The compiler sees distinct virtual addresses. Observe the OS-created
    // aliases through volatile pointers rather than letting it reuse values
    // from a different virtual address's C++ memory object.
    volatile uint8_t* aliases = view;
    for (size_t offset : {size_t(0), size_t(4096), ram.size() - 1}) {
      assert(aliases[offset] == 0);
      assert(aliases[0x80000000ull + offset] == 0);
      assert(aliases[0xC0000000ull + offset] == 0);
      aliases[0x80000000ull + offset] = 0x45;
      assert(aliases[offset] == 0x45 && aliases[0xC0000000ull + offset] == 0x45);
      aliases[0xC0000000ull + offset] = 0xA7;
      assert(aliases[offset] == 0xA7 && aliases[0x80000000ull + offset] == 0xA7);
    }
    volatile uint8_t* cache = ram.locked_cache();
    assert(cache == view + 0xE0000000ull);
    assert(cache[0] == 0 && cache[LinuxGuestRam::kLockedCacheSize - 1] == 0);
    cache[0] = 0x31;
    cache[LinuxGuestRam::kLockedCacheSize - 1] = 0x72;
    assert(aliases[0xE0000000ull] == 0x31);
    assert(aliases[0xE0003FFFull] == 0x72);
    assert(aliases[0] == 0xA7);  // locked cache uses a separate backing-file region
    // Exercise the production fixed-opcode asm helpers without a separate
    // volatile observer or compiler barrier masking alias-ordering mistakes.
    check_fastmem_mixed_access(view);
    check_fastmem_alias<uint8_t>(view, 0x21, 0xB4);
    check_fastmem_alias<uint16_t>(view, 0x1234, 0xABCD);
    check_fastmem_alias<uint32_t>(view, 0x12345678u, 0xABCDEF12u);
    check_fastmem_alias<uint64_t>(view, 0x123456789ABCDEF0ull, 0xFEDCBA9876543210ull);
    expect_guard_fault(view + ram.size());
    expect_guard_fault(view + 0xE0004000ull);
    expect_guard_fault(view + LinuxGuestRam::kAddressSpaceSize);
  }
  unsigned char resident = 0;
  for (uint64_t offset : {uint64_t(0), uint64_t(0x80000000), uint64_t(0xC0000000),
                          uint64_t(0xE0000000), LinuxGuestRam::kAddressSpaceSize}) {
    errno = 0;
    assert(::mincore(released_view + offset, size_t(::sysconf(_SC_PAGESIZE)), &resident) == -1);
    assert(errno == ENOMEM);
  }
  ::setenv("STANDALONE_FASTMEM", "0", 1);
  {
    LinuxGuestRam ram;
    assert(!ram.fastmem_view());
    assert(ram[0] == 0 && ram[ram.size() - 1] == 0);
    ram[0] = 3;
    ram[ram.size() - 1] = 4;
    ram.locked_cache()[0] = 5;
    ram.locked_cache()[LinuxGuestRam::kLockedCacheSize - 1] = 6;
    assert(ram[0] == 3 && ram[ram.size() - 1] == 4);
    assert(ram.locked_cache()[0] == 5);
    assert(ram.locked_cache()[LinuxGuestRam::kLockedCacheSize - 1] == 6);
  }
  // A real reservation failure must use the same ordinary-RAM fallback.
  const pid_t limited = ::fork();
  assert(limited >= 0);
  if (!limited) {
    ::setenv("STANDALONE_FASTMEM", "1", 1);
    rlimit address_limit{};
    assert(::getrlimit(RLIMIT_AS, &address_limit) == 0);
    address_limit.rlim_cur = std::min<rlim_t>(128u * 1024u * 1024u, address_limit.rlim_max);
    assert(::setrlimit(RLIMIT_AS, &address_limit) == 0);
    LinuxGuestRam ram;
    assert(!ram.fastmem_view() && ram.size() == LinuxGuestRam::kRamSize);
    assert(ram[0] == 0 && ram[ram.size() - 1] == 0);
    ram.locked_cache()[LinuxGuestRam::kLockedCacheSize - 1] = 9;
    assert(ram.locked_cache()[LinuxGuestRam::kLockedCacheSize - 1] == 9);
    ::_exit(0);
  }
  int limited_status = 0;
  assert(::waitpid(limited, &limited_status, 0) == limited);
  assert(WIFEXITED(limited_status) && WEXITSTATUS(limited_status) == 0);
  if (had_previous) ::setenv("STANDALONE_FASTMEM", saved.c_str(), 1);
  else ::unsetenv("STANDALONE_FASTMEM");
  std::puts("Linux guest RAM aliases, guards, locked cache and fallback passed");
}
#else
int main() { return 0; }
#endif
