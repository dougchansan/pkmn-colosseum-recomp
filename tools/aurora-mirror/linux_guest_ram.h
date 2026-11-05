#pragma once

#if defined(__linux__) && defined(__x86_64__)

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

// MEM1's three aliases and locked cache share one backing file. The module's
// existing fastmem decoder handles accesses to the inaccessible holes; this
// class installs no signal handler. A failed reservation keeps ordinary RAM.
class LinuxGuestRam {
public:
  static constexpr size_t kRamSize = 24u * 1024u * 1024u;
  static constexpr size_t kLockedCacheSize = 16u * 1024u;
  static constexpr uint64_t kAddressSpaceSize = uint64_t{1} << 32;

  LinuxGuestRam() {
    const char* enabled = std::getenv("STANDALONE_FASTMEM");
    if ((!enabled || std::strcmp(enabled, "0") != 0) && allocate_view())
      return;
    fallback_.resize(kRamSize);
  }

  ~LinuxGuestRam() {
    if (view_)
      ::munmap(view_, reservation_size_);
  }

  LinuxGuestRam(const LinuxGuestRam&) = delete;
  LinuxGuestRam& operator=(const LinuxGuestRam&) = delete;
  LinuxGuestRam(LinuxGuestRam&&) = delete;
  LinuxGuestRam& operator=(LinuxGuestRam&&) = delete;

  uint8_t* data() noexcept { return view_ ? view_ : fallback_.data(); }
  const uint8_t* data() const noexcept { return view_ ? view_ : fallback_.data(); }
  constexpr size_t size() const noexcept { return kRamSize; }
  uint8_t& operator[](size_t index) noexcept { return data()[index]; }
  const uint8_t& operator[](size_t index) const noexcept { return data()[index]; }
  uint8_t* fastmem_view() const noexcept { return view_; }
  uint8_t* locked_cache() noexcept {
    return view_ ? view_ + 0xE0000000ull : fallback_locked_cache_.data();
  }
  const uint8_t* locked_cache() const noexcept {
    return view_ ? view_ + 0xE0000000ull : fallback_locked_cache_.data();
  }

private:
  bool allocate_view() noexcept {
    const long page = ::sysconf(_SC_PAGESIZE);
    // Require exact cache page boundaries so bytes after its 16 KiB stay
    // inaccessible. x86_64 Linux normally uses 4 KiB base pages.
    if (page <= 0 || kRamSize % size_t(page) || kLockedCacheSize % size_t(page))
      return false;
    const size_t reservation_size = size_t(kAddressSpaceSize) + size_t(page);
    void* reservation = ::mmap(nullptr, reservation_size, PROT_NONE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (reservation == MAP_FAILED)
      return false;
    if (!reservation) {
      ::munmap(reservation, reservation_size);
      return false;
    }
    auto* base = static_cast<uint8_t*>(reservation);
    // Use the kernel interface so older glibc without a memfd_create wrapper
    // can still use the existing Linux facility. Failure uses the fallback.
    const int fd = int(::syscall(SYS_memfd_create, "colosseum-guest-ram", MFD_CLOEXEC));
    if (fd < 0) {
      ::munmap(reservation, reservation_size);
      return false;
    }
    bool mapped = ::ftruncate(fd, off_t(kRamSize + kLockedCacheSize)) == 0;
    const auto alias = [&](uint64_t address, size_t bytes, off_t backing_offset) {
      // MAP_FIXED may replace only pages inside this class's own reservation.
      // The final host page remains a guard for accesses crossing EA=FFFFFFFF.
      if (address > kAddressSpaceSize || bytes > kAddressSpaceSize - address ||
          address % uint64_t(page) || bytes % size_t(page))
        return false;
      void* target = base + size_t(address);
      return ::mmap(target, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED,
                     fd, backing_offset) == target;
    };
    if (mapped)
      mapped = alias(0, kRamSize, 0) && alias(0x80000000ull, kRamSize, 0) &&
               alias(0xC0000000ull, kRamSize, 0) &&
               alias(0xE0000000ull, kLockedCacheSize, off_t(kRamSize));
    ::close(fd);  // mappings retain the backing object
    if (!mapped) {
      ::munmap(reservation, reservation_size);
      return false;
    }
    view_ = base;
    reservation_size_ = reservation_size;
    return true;
  }

  uint8_t* view_ = nullptr;
  size_t reservation_size_ = 0;
  std::vector<uint8_t> fallback_;
  std::array<uint8_t, kLockedCacheSize> fallback_locked_cache_{};
};

#endif  // Linux x86_64
