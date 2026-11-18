#pragma once

// Way back for a device that cannot be flashed over USB. When a start that
// follows a crash or a watchdog reset fails the same way before the firmware
// ran for a few seconds, the bootloader is switched to the firmware in the
// other app slot, the one that was running before the update. The other slot
// is used only when it holds a complete image.
//
// It cannot help when the firmware fails before setup(), or hangs without a
// reset. A normal start writes to flash only to clear a count that an
// interrupted start left behind.
namespace boot_fallback {

// Call early in setup(), once the serial log is up. Does not return when it
// switches to the other slot. True when the previous start ended in a crash or
// a watchdog reset; this covers watchdog resets that left no crash report.
bool checkAtBoot();

// Call on every pass of loop(). Forgets the failed starts once the firmware ran long enough.
void noteLoopPass();

}  // namespace boot_fallback
