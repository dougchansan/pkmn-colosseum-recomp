// A GameCube memory card on EXI, as Dolphin's CEXIMemoryCard models it, backed
// by a raw card image (Dolphin's .raw format: the card's flash, byte for byte).
#pragma once

#include <cstdint>
#include <string>

// Opens the card image at `path`, or creates it formatted as Dolphin creates a
// new card (2043 blocks, header serial derived from `flash_id`). Returns false
// when the image cannot be read or created.
bool memcard_open(const std::string& path, const uint8_t flash_id[12], uint32_t language);
// The first 20 bytes of the card (serial and format time), from which the SRAM
// flash ID that unlocks this card is derived.
const uint8_t* memcard_header();

// Chip select changed (the device's CS bit in EXI channel 0's status), at
// guest time `cycles`.
void memcard_set_cs(bool selected, uint64_t cycles);
// One immediate transfer of `length` bytes (1-4), MSB first, as IEXIDevice does.
uint32_t memcard_imm_read(uint32_t length);
void memcard_imm_write(uint32_t data, uint32_t length);
// DMA at the address the preceding immediate command set up.
void memcard_dma_read(uint8_t* dst, uint32_t length);
void memcard_dma_write(const uint8_t* src, uint32_t length);

// Advances the card's command timer to guest time `cycles`; true when a
// command finished just now (its interrupt may need raising).
bool memcard_tick(uint64_t cycles);
uint64_t memcard_next_event();  // absolute command-completion cycle, or 0
// Whether the card asserts its EXI interrupt (an erase or program finished).
bool memcard_interrupt();

// Writes the image back to disk if it changed; without `force`, only once
// writes have settled for a second.
void memcard_flush(bool force);
