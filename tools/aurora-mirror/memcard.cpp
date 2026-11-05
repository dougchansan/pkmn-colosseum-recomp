// The memory card in slot A: the command protocol of Dolphin's CEXIMemoryCard
// (Core/HW/EXI/EXI_DeviceMemoryCard.cpp) over a raw image as MemoryCard
// (Core/HW/GCMemcard/GCMemcardRaw.cpp) keeps it, and a new image formatted as
// GCMemcard::Format does.

#include "gxruntime/utf8_file.h"
#include "memcard.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <SDL3/SDL.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace {

constexpr uint32_t kBlockSize = 0x2000;
constexpr uint32_t kSizeToMbit = 1024 * 8 * 16;  // bytes per megabit of card
constexpr uint16_t kDefaultMbits = 0x80;          // Memory Card 2043, Dolphin's default
constexpr uint32_t kMbitToBlocks = (1024 * 1024) / (kBlockSize * 8);
constexpr uint32_t kSystemBlocks = 5;
constexpr uint64_t kGcEpoch = 946684800;  // 2000-01-01, in Unix seconds
constexpr uint64_t kCmdDoneCycles = 5000;

constexpr uint8_t kStatusBusy = 0x80, kStatusUnlocked = 0x40, kStatusEraseError = 0x10,
                  kStatusProgramError = 0x08, kStatusReady = 0x01;

enum Command : uint8_t {
  NintendoID = 0x00,
  ReadArray = 0x52,
  SetInterrupt = 0x81,
  ReadStatus = 0x83,
  ReadID = 0x85,
  ClearStatus = 0x89,
  SectorErase = 0xF1,
  PageProgram = 0xF2,
  ChipErase = 0xF4,
};

std::string g_path;
std::vector<uint8_t> g_data;
uint32_t g_size = 0;   // bytes, a power of two
uint32_t g_card_id = 0;  // size in megabits, what the Nintendo ID command reports
const uint16_t kFlashCardId = 0xC221;  // a Nintendo-brand card (ReadID)

uint8_t g_command = NintendoID;
uint8_t g_status = kStatusBusy | kStatusUnlocked | kStatusReady;
uint32_t g_position = 0, g_address = 0;
uint8_t g_interrupt_switch = 0;
bool g_interrupt_set = false;
uint8_t g_program[128] = {};
uint64_t g_cmd_done_at = 0;  // 0: none pending

bool g_dirty = false;
std::chrono::steady_clock::time_point g_last_write;

void be16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
void be32(uint8_t* p, uint32_t v) { be16(p, uint16_t(v >> 16)); be16(p + 2, uint16_t(v)); }
void be64(uint8_t* p, uint64_t v) { be32(p, uint32_t(v >> 32)); be32(p + 4, uint32_t(v)); }

// The additive and inverse checksums over big-endian halfwords (CalculateMemcardChecksums).
void fix_checksums(uint8_t* area, size_t size, uint8_t* out) {
  uint16_t sum = 0, inv = 0;
  for (size_t i = 0; i < size; i += 2) {
    const uint16_t d = uint16_t(area[i] << 8 | area[i + 1]);
    sum = uint16_t(sum + d);
    inv = uint16_t(inv + (d ^ 0xFFFF));
  }
  be16(out, sum == 0xFFFF ? 0 : sum);
  be16(out + 2, inv == 0xFFFF ? 0 : inv);
}

// GCMemcard::Format: header, two empty directories, two block allocation maps.
void format(uint8_t* card, const uint8_t flash_id[12], uint16_t mbits, uint32_t language,
            uint64_t format_time) {
  uint8_t* header = card;
  std::memset(header, 0xFF, kBlockSize);
  uint64_t rand = format_time;
  for (int i = 0; i < 12; ++i) {
    rand = ((rand * 0x41C64E6Dull) + 0x3039ull) >> 16;
    header[i] = uint8_t(flash_id[i] + uint32_t(rand));
    rand = ((rand * 0x41C64E6Dull) + 0x3039ull) >> 16;
    rand &= 0x7FFFull;
  }
  be64(header + 0x0C, format_time);
  be32(header + 0x14, 0);         // SRAM bias
  be32(header + 0x18, language);  // SRAM language
  be32(header + 0x1C, 0);         // DTV status
  be16(header + 0x20, 0);         // formatted in slot A
  be16(header + 0x22, mbits);
  be16(header + 0x24, 0);         // encoding: Windows-1252
  fix_checksums(header, 0x1FC, header + 0x1FC);

  uint8_t* dir = card + kBlockSize;
  std::memset(dir, 0xFF, kBlockSize);
  be16(dir + 0x1FFA, 0);  // update counter
  fix_checksums(dir, 0x1FFC, dir + 0x1FFC);
  std::memcpy(card + 2 * kBlockSize, dir, kBlockSize);

  uint8_t* bat = card + 3 * kBlockSize;
  std::memset(bat, 0, kBlockSize);
  be16(bat + 6, uint16_t(mbits * kMbitToBlocks - kSystemBlocks));  // free blocks
  be16(bat + 8, 4);                                               // last allocated block
  fix_checksums(bat + 4, kBlockSize - 4, bat);
  std::memcpy(card + 4 * kBlockSize, bat, kBlockSize);
}

void make_dirty() {
  g_dirty = true;
  g_last_write = std::chrono::steady_clock::now();
}

void read_card(uint32_t address, uint8_t* dst, uint32_t length) {
  for (uint32_t i = 0; i < length; ++i)
    dst[i] = g_data[(address + i) & (g_size - 1)];
}

void write_card(uint32_t address, const uint8_t* src, uint32_t length) {
  for (uint32_t i = 0; i < length; ++i)
    g_data[(address + i) & (g_size - 1)] = src[i];
  make_dirty();
}

void cmd_done_later(uint64_t now) { g_cmd_done_at = now + kCmdDoneCycles; }

// CEXIMemoryCard::TransferByte.
void transfer_byte(uint8_t& byte) {
  if (g_position == 0) {
    g_command = byte;
    byte = 0xFF;
    if (g_command == ClearStatus) {
      g_status &= ~(kStatusProgramError | kStatusEraseError);
      g_status |= kStatusReady;
      g_interrupt_set = false;
      g_position = 0;
    }
  } else {
    switch (g_command) {
    case NintendoID:
      if (g_position == 1)
        byte = 0x80;  // dummy cycle
      else
        byte = uint8_t(g_card_id >> (24 - (((g_position - 2) & 3) * 8)));
      break;
    case ReadArray:
      switch (g_position) {
      case 1: g_address = uint32_t(byte) << 17; byte = 0xFF; break;
      case 2: g_address |= uint32_t(byte) << 9; break;
      case 3: g_address |= uint32_t(byte & 3) << 7; break;
      case 4: g_address |= byte & 0x7F; break;
      }
      if (g_position > 1) {
        read_card(g_address & (g_size - 1), &byte, 1);
        if (g_position >= 9)  // the pointer wraps within the 512-byte page
          g_address = (g_address & ~0x1FFu) | ((g_address + 1) & 0x1FF);
      }
      break;
    case ReadStatus:
      byte = g_status;
      break;
    case ReadID:
      if (g_position == 1)
        byte = uint8_t(kFlashCardId >> 8);
      else
        byte = uint8_t((g_position & 1) ? kFlashCardId : (kFlashCardId >> 8));
      break;
    case SectorErase:
      switch (g_position) {
      case 1: g_address = uint32_t(byte) << 17; break;
      case 2: g_address |= uint32_t(byte) << 9; break;
      }
      byte = 0xFF;
      break;
    case SetInterrupt:
      if (g_position == 1)
        g_interrupt_switch = byte;
      byte = 0xFF;
      break;
    case ChipErase:
      byte = 0xFF;
      break;
    case PageProgram:
      switch (g_position) {
      case 1: g_address = uint32_t(byte) << 17; break;
      case 2: g_address |= uint32_t(byte) << 9; break;
      case 3: g_address |= uint32_t(byte & 3) << 7; break;
      case 4: g_address |= byte & 0x7F; break;
      }
      if (g_position >= 5)
        g_program[(g_position - 5) & 0x7F] = byte;
      byte = 0xFF;
      break;
    default:
      std::printf("memcard: unknown command 0x%02X\n", g_command);
      byte = 0xFF;
      break;
    }
  }
  ++g_position;
}

// Copies a folder of .gci saves (Dolphin's GCI-folder card, as the launcher keeps
// it) into a freshly formatted card image: one directory entry and a block chain
// per save, the newest directory and BAT written over both copies.
void import_gci_folder(uint8_t* card, uint32_t size, const std::string& folder) {
  namespace fs = std::filesystem;
  std::error_code ec;
  const fs::path root = fs::u8path(folder);
  if (folder.empty() || !fs::is_directory(root, ec))
    return;
  const uint32_t total_blocks = size / kBlockSize;
  uint8_t* dir = card + kBlockSize;
  uint8_t* bat = card + 3 * kBlockSize;
  auto rd16 = [](const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); };
  std::vector<fs::path> saves;
  for (const auto& entry : fs::directory_iterator(root, ec))
    if (entry.is_regular_file(ec) && entry.path().extension() == ".gci")
      saves.push_back(entry.path());
  std::sort(saves.begin(), saves.end());
  int imported = 0;
  for (const fs::path& path : saves) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> gci((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (gci.size() < 0x40)
      continue;
    const uint16_t count = rd16(gci.data() + 0x38);
    if (count == 0 || gci.size() != 0x40 + size_t(count) * kBlockSize)
      continue;
    int slot = -1;
    for (int i = 0; i < 127 && slot < 0; ++i)
      if (dir[i * 0x40] == 0xFF && dir[i * 0x40 + 1] == 0xFF && dir[i * 0x40 + 2] == 0xFF &&
          dir[i * 0x40 + 3] == 0xFF)
        slot = i;
    std::vector<uint16_t> chain;
    for (uint32_t b = kSystemBlocks; b < total_blocks && chain.size() < count; ++b)
      if (rd16(bat + 0x0A + (b - kSystemBlocks) * 2) == 0)
        chain.push_back(uint16_t(b));
    if (slot < 0 || chain.size() < count) {
      std::fprintf(stderr, "memcard: no room for %s\n", path.filename().u8string().c_str());
      continue;
    }
    for (size_t i = 0; i < chain.size(); ++i) {
      const uint16_t next = i + 1 < chain.size() ? chain[i + 1] : 0xFFFF;
      be16(bat + 0x0A + (chain[i] - kSystemBlocks) * 2, next);
      std::memcpy(card + size_t(chain[i]) * kBlockSize, gci.data() + 0x40 + i * kBlockSize, kBlockSize);
    }
    be16(bat + 6, uint16_t(rd16(bat + 6) - count));
    be16(bat + 8, chain.back());
    std::memcpy(dir + slot * 0x40, gci.data(), 0x40);
    be16(dir + slot * 0x40 + 0x36, chain.front());
    ++imported;
  }
  if (!imported)
    return;
  be16(bat + 4, uint16_t(rd16(bat + 4) + 1));
  fix_checksums(bat + 4, kBlockSize - 4, bat);
  std::memcpy(card + 4 * kBlockSize, bat, kBlockSize);
  be16(dir + 0x1FFA, uint16_t(rd16(dir + 0x1FFA) + 1));
  fix_checksums(dir, 0x1FFC, dir + 0x1FFC);
  std::memcpy(card + 2 * kBlockSize, dir, kBlockSize);
  std::printf("memcard: imported %d save(s) from %s\n", imported, folder.c_str());
}

}  // namespace

bool memcard_open(const std::string& path, const uint8_t flash_id[12], uint32_t language) {
  g_path = path;
  if (FILE* f = dol_utf8_fopen(path.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size < long(kSizeToMbit * 4) || (size & (size - 1))) {
      std::fprintf(stderr, "memcard: %s is %ld bytes, not a raw card image\n", path.c_str(), size);
      std::fclose(f);
      return false;
    }
    g_size = uint32_t(size);
    g_data.assign(g_size, 0xFF);
    const size_t got = std::fread(g_data.data(), 1, g_size, f);
    std::fclose(f);
    if (got != g_size) {
      std::fprintf(stderr, "memcard: could not read %s\n", path.c_str());
      return false;
    }
    g_card_id = g_size / kSizeToMbit;
    std::printf("memcard: slot A is %s (%u Mbit)\n", path.c_str(), g_card_id);
  } else {
    g_card_id = kDefaultMbits;
    g_size = g_card_id * kSizeToMbit;
    g_data.assign(g_size, 0xFF);
    const uint64_t format_time = uint64_t(std::time(nullptr)) - kGcEpoch;
    format(g_data.data(), flash_id, kDefaultMbits, language, format_time);
    // A launcher keeps saves as a GCI folder; a new card starts with copies of them.
    import_gci_folder(g_data.data(), g_size, dol_utf8::environment("STANDALONE_IMPORT_GCI"));
    make_dirty();
    memcard_flush(true);
    std::printf("memcard: created %s (%u Mbit, formatted)\n", path.c_str(), g_card_id);
  }
  return true;
}

const uint8_t* memcard_header() { return g_data.data(); }

void memcard_set_cs(bool selected, uint64_t cycles) {
  if (selected) {
    g_position = 0;
    return;
  }
  switch (g_command) {
  case SectorErase:
    if (g_position > 2) {
      const uint32_t address = g_address & (g_size - 1);
      if (address & (kBlockSize - 1)) {
        std::printf("memcard: sector erase at unaligned 0x%X\n", address);
      } else {
        std::memset(&g_data[address], 0xFF, kBlockSize);
        make_dirty();
      }
      g_status |= kStatusBusy;
      g_status &= ~kStatusReady;
      cmd_done_later(cycles);
    }
    break;
  case ChipErase:
    if (g_position > 2) {
      std::memset(g_data.data(), 0xFF, g_size);
      make_dirty();
      g_status &= ~kStatusBusy;
    }
    break;
  case PageProgram:
    if (g_position >= 5) {
      uint32_t count = g_position - 5;
      uint32_t i = 0;
      g_status &= ~kStatusBusy;
      while (count--) {
        write_card(g_address, &g_program[i++], 1);
        i &= 127;
        g_address = (g_address & ~0x1FFu) | ((g_address + 1) & 0x1FF);
      }
      cmd_done_later(cycles);
    }
    break;
  default:
    break;
  }
}

uint32_t memcard_imm_read(uint32_t length) {
  uint32_t result = 0;
  for (uint32_t position = 0; position < length; ++position) {
    uint8_t byte = 0;
    transfer_byte(byte);
    result |= uint32_t(byte) << (24 - position * 8);
  }
  return result;
}

void memcard_imm_write(uint32_t data, uint32_t length) {
  while (length--) {
    uint8_t byte = uint8_t(data >> 24);
    transfer_byte(byte);
    data <<= 8;
  }
}

void memcard_dma_read(uint8_t* dst, uint32_t length) { read_card(g_address, dst, length); }

void memcard_dma_write(const uint8_t* src, uint32_t length) { write_card(g_address, src, length); }

uint64_t memcard_next_event() { return g_cmd_done_at; }

bool memcard_tick(uint64_t cycles) {
  if (!g_cmd_done_at || cycles < g_cmd_done_at)
    return false;
  g_cmd_done_at = 0;
  g_status |= kStatusReady;
  g_status &= ~kStatusBusy;
  g_interrupt_set = true;
  return true;
}

bool memcard_interrupt() { return g_interrupt_switch && g_interrupt_set; }

void memcard_flush(bool force) {
  if (!g_dirty || g_data.empty())
    return;
  if (!force && std::chrono::steady_clock::now() - g_last_write < std::chrono::seconds(1))
    return;
  // Beside the image first, then over it: a failed write (a full disk) leaves
  // the previous image as it was.
  const std::string temp = g_path + ".tmp";
  FILE* f = dol_utf8_fopen(temp.c_str(), "wb");
  if (!f) {
    std::fprintf(stderr, "memcard: could not write %s\n", temp.c_str());
    return;
  }
  const bool wrote = std::fwrite(g_data.data(), 1, g_size, f) == g_size;
  const bool closed = std::fclose(f) == 0;
  const bool replaced = wrote && closed && SDL_RenamePath(temp.c_str(), g_path.c_str());
  if (!replaced) {
    std::fprintf(stderr, "memcard: could not write %s\n", g_path.c_str());
    SDL_RemovePath(temp.c_str());
    return;
  }
  g_dirty = false;
  std::printf("memcard: wrote %s\n", g_path.c_str());
}
