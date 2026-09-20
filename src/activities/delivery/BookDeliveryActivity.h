#pragma once

#include <I18n.h>

#include "activities/Activity.h"

/**
 * How this delivery run was started. The trigger decides whether the run may
 * force a re-download and how the result screen is dismissed.
 */
enum class DeliveryTrigger {
  CatchUp,  // Pushed at boot because a delivery was owed; auto-dismisses
  Manual    // Settings action; forces a fresh download and waits for Back
};

/**
 * BookDeliveryActivity - fetches the day's EPUB from a configured URL.
 *
 * 1. Ensure WiFi (reusing the normal selection flow when not connected)
 * 2. Sync the clock if it has never been set - the local date names the file
 * 3. Re-check that a delivery is owed, now that the time is known
 * 4. Download to /YYYY-MM-DD.epub and verify it is really a zip
 * 5. Stamp APP_STATE.lastDeliveryFetch so the rest of the day stays quiet
 */
class BookDeliveryActivity final : public Activity {
 public:
  explicit BookDeliveryActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, DeliveryTrigger trigger)
      : Activity("BookDelivery", renderer, mappedInput), trigger(trigger) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool preventAutoSleep() override { return true; }
  bool skipLoopDelay() override { return true; }

 private:
  enum State { Connecting, Working, Finished };

  const DeliveryTrigger trigger;
  State state = Working;
  StrId outcome = StrId::STR_DELIVERY_DOWNLOADING;
  bool succeeded = false;

  // Flipped by the progress callback on a Back press or once the transfer
  // outruns its budget; HttpDownloader checks it between chunks and aborts.
  bool cancelDownload = false;
  unsigned long downloadStartMs = 0;
  unsigned long settledAtMs = 0;
  int percentDone = 0;
  int lastRenderedPercent = -1;

  void launchWifiSelection();
  void runDelivery();
  bool downloadTodaysBook(const std::string& destPath);
  static bool looksLikeEpub(const std::string& path);
  void settle(StrId message, bool ok);

  // A book is a few hundred KB at worst; past this the far end has stalled.
  static constexpr unsigned long DOWNLOAD_BUDGET_MS = 180000;
  // Repaint thresholds, matching the OPDS downloader: e-ink refreshes are slow
  // and costly, so progress is coarse on purpose.
  static constexpr int PROGRESS_STEP_PERCENT = 5;
  static constexpr unsigned long PROGRESS_MIN_UPDATE_MS = 5000;
  // Long enough to read a failure on the way past, short enough not to nag.
  static constexpr unsigned long FAILURE_NOTICE_MS = 2500;
};
