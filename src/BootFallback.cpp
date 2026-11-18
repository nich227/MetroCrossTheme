#include "BootFallback.h"

#ifdef SIMULATOR

namespace boot_fallback {
bool checkAtBoot() { return false; }
void noteLoopPass() {}
}  // namespace boot_fallback

#else

#include <Arduino.h>
#include <Logging.h>
#include <Memory.h>
#include <Preferences.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>
#include <esp_system.h>

#include "BootImageCheck.h"
#include "network/OtaBootSwitch.h"

namespace boot_fallback {
namespace {

constexpr char NVS_NAMESPACE[] = "bootfb";
constexpr char NVS_KEY_FAILED_STARTS[] = "fails";
constexpr uint8_t MAX_FAILED_STARTS = 2;
// The firmware counts as started once loop() ran for this long.
constexpr unsigned long HEALTHY_AFTER_MS = 3000;

// Set when this start was counted, so the count must be cleared once the firmware is up.
bool countPending = false;
unsigned long firstLoopPassMs = 0;

bool endedInFailure(const esp_reset_reason_t reason) {
  return reason == ESP_RST_PANIC || reason == ESP_RST_CPU_LOCKUP || reason == ESP_RST_INT_WDT ||
         reason == ESP_RST_TASK_WDT || reason == ESP_RST_WDT;
}

// An update that stopped half way leaves a good header over a broken body, and
// the update paths write raw images that nothing has checked since. So the
// whole image is read and checked here before the bootloader is pointed at it.
bool holdsCompleteImage(const esp_partition_t* partition) {
  if (!partition) return false;
  auto scratch = makeUniqueNoThrow<uint8_t[]>(boot_image::CHUNK_SIZE);
  if (!scratch) {
    LOG_ERR("BOOT", "OOM: image check buffer (%u bytes)", static_cast<unsigned>(boot_image::CHUNK_SIZE));
    return false;
  }
  size_t reads = 0;
  const auto read = [partition, &reads](const size_t offset, uint8_t* buffer, const size_t length) {
    // The check reads several megabytes; let the idle task run so its watchdog stays fed.
    if ((++reads & 0x0F) == 0) delay(1);
    return esp_partition_read(partition, offset, buffer, length) == ESP_OK;
  };
  return boot_image::isComplete(read, partition->size, scratch.get());
}

void storeFailedStarts(const uint8_t count) {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return;
  prefs.putUChar(NVS_KEY_FAILED_STARTS, count);
  prefs.end();
}

uint8_t loadFailedStarts() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return 0;
  const uint8_t count = prefs.getUChar(NVS_KEY_FAILED_STARTS, 0);
  prefs.end();
  return count;
}

}  // namespace

bool checkAtBoot() {
  if (!endedInFailure(esp_reset_reason())) {
    // A start that was cut short left a count behind. Two failures only count when one follows the other.
    if (loadFailedStarts() != 0) storeFailedStarts(0);
    return false;
  }

  const uint8_t failedStarts = loadFailedStarts() + 1;
  if (failedStarts < MAX_FAILED_STARTS) {
    storeFailedStarts(failedStarts);
    countPending = true;
    return true;
  }

  // Start the count again in every case: the other firmware may be a build with this same check.
  storeFailedStarts(0);
  const esp_partition_t* other = esp_ota_get_next_update_partition(nullptr);
  // A layout with one app slot names the running slot here.
  if (!other || other == esp_ota_get_running_partition()) return true;
  if (!holdsCompleteImage(other)) {
    LOG_ERR("BOOT", "Fallback: no complete firmware in %s", other->label);
    return true;
  }
  if (!ota_boot::switchTo(other)) {
    LOG_ERR("BOOT", "Fallback: switch to %s failed", other->label);
    return true;
  }
  LOG_ERR("BOOT", "Fallback: %u failed starts, restarting from %s", static_cast<unsigned>(failedStarts), other->label);
  esp_restart();
  return true;
}

void noteLoopPass() {
  if (!countPending) return;
  const unsigned long now = millis();
  if (firstLoopPassMs == 0) {
    firstLoopPassMs = now == 0 ? 1 : now;
    return;
  }
  if (now - firstLoopPassMs < HEALTHY_AFTER_MS) return;
  countPending = false;
  storeFailedStarts(0);
}

}  // namespace boot_fallback

#endif
