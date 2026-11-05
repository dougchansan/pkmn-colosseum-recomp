// AX DSP high-level emulation for colosseum-standalone: a self-contained port
// of Dolphin's AXUCode (GameCube AX), its accelerator and voice mixer
// (lib/ModernGekko/vendor/dolphin/Source/Core/Core/HW/DSPHLE/UCodes/AX*.{h,cpp},
// Core/DSP/DSPAccelerator.cpp). Copyright Dolphin Emulator Project,
// GPL-2.0-or-later.
#pragma once

#include <cstdint>

struct AxHostInterface {
  uint8_t* ram;        // MEM1, big-endian guest bytes; guest address & 0x1FFFFFFF indexes it
  uint32_t ram_size;
  uint8_t* aram;       // ARAM, what the accelerator streams samples from
  uint32_t aram_size;  // a power of two
  // DSP -> CPU mail. interrupt follows Dolphin's CMailHandler::PushMail: raise
  // the DSP interrupt now if no mail is queued, else when the queue head is read.
  void (*push_mail)(uint32_t mail, bool interrupt);
};

// The AX task has booted (after __DSP_boot_task's init vector mail). crc is the
// ector hash of the task's IRAM image (picks the PB layout / mixer_control
// meaning, as in Dolphin's UCodeFactory). Pushes DSP_INIT like
// AXUCode::InitializeShared.
void ax_init(const AxHostInterface& host, uint32_t crc);
// Every CPU -> DSP mail once the AX task runs.
void ax_handle_mail(uint32_t mail);
// Per audio frame, like AXUCode::Update (resume mail after a ucode switch).
void ax_update();
// False once the CPU sent MAIL_RESET (the DSP is back in its ROM).
bool ax_active();
// The ector hash Dolphin uses to identify ucodes (Common::HashEctor).
uint32_t ax_hash_ector(const uint8_t* data, uint32_t length);
// Counters for status lines.
uint64_t ax_command_lists();
