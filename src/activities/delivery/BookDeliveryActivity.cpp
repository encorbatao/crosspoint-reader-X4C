#include "BookDeliveryActivity.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LibraryBuilder.h>
#include <Logging.h>
#include <WiFi.h>

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "MappedInputManager.h"
#include "SilentRestart.h"
#include "activities/network/WifiSelectionActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "network/HttpDownloader.h"
#include "util/DeliverySchedule.h"

namespace {

// Every EPUB is a zip. Checking the local-file-header magic is the cheapest way
// to tell a real book from a server that answered 200 with an HTML error page.
constexpr char ZIP_MAGIC[] = {'P', 'K'};

time_t nowUtc() {
  time_t now = 0;
  time(&now);
  return now;
}

}  // namespace

void BookDeliveryActivity::onEnter() {
  Activity::onEnter();

  if (strlen(SETTINGS.bookDeliveryUrl) == 0) {
    settle(StrId::STR_DELIVERY_NO_URL, false);
    return;
  }

  if (WiFi.status() == WL_CONNECTED) {
    state = Working;
    requestUpdate();
    return;
  }

  state = Connecting;
  launchWifiSelection();
}

void BookDeliveryActivity::launchWifiSelection() {
  // autoConnect: saved networks are tried without asking, which is what an
  // unattended-ish boot fetch needs. The picker only surfaces if that fails.
  startActivityForResult(std::make_unique<WifiSelectionActivity>(renderer, mappedInput, true),
                         [this](const ActivityResult& result) {
                           if (result.isCancelled || WiFi.status() != WL_CONNECTED) {
                             settle(StrId::STR_DELIVERY_FAILED, false);
                             return;
                           }
                           state = Working;
                           requestUpdate();
                         });
}

bool BookDeliveryActivity::looksLikeEpub(const std::string& path) {
  HalFile file;
  if (!Storage.openFileForRead("DLV", path, file)) {
    return false;
  }

  char magic[sizeof(ZIP_MAGIC)] = {};
  const int read = file.read(magic, sizeof(magic));
  file.close();

  return read == static_cast<int>(sizeof(magic)) && memcmp(magic, ZIP_MAGIC, sizeof(magic)) == 0;
}

bool BookDeliveryActivity::downloadTodaysBook(const std::string& destPath) {
  // Already delivered. A manual run downloads anyway: it exists to prove the
  // whole path works, and reporting a skip would prove nothing.
  if (trigger != DeliveryTrigger::Manual && Storage.exists(destPath.c_str())) {
    LOG_INF("DLV", "Already have today's book");
    deliveredPath = destPath;
    return true;
  }

  // Download beside the target, never onto it: downloadToFile() deletes the
  // destination before it even opens the connection, so writing straight to
  // destPath would destroy a book already delivered today the moment the URL
  // 404s. The swap at the end is also what makes a power cut mid-transfer
  // leave a .part behind rather than a half-written "book".
  const std::string tempPath = destPath + ".part";

  downloadStartMs = millis();
  cancelDownload = false;
  percentDone = 0;
  lastRenderedPercent = -1;

  unsigned long lastProgressUpdateMs = 0;
  const auto result = HttpDownloader::downloadToFile(
      SETTINGS.bookDeliveryUrl, tempPath,
      [this, &lastProgressUpdateMs](const size_t downloaded, const size_t total) {
        // The activity loop is blocked for the whole download, so input is
        // pumped here - otherwise Back is dead and the device looks hung.
        mappedInput.update(true);
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
          cancelDownload = true;
        }

        // HttpDownloader has no wall-clock bound of its own; bounding it here
        // keeps a stalled server from draining the battery unattended.
        if (millis() - downloadStartMs > DOWNLOAD_BUDGET_MS) {
          LOG_ERR("DLV", "Download budget exhausted, aborting");
          cancelDownload = true;
        }

        percentDone = total > 0 ? static_cast<int>(static_cast<uint64_t>(downloaded) * 100 / total) : 0;
        const unsigned long now = millis();
        if (percentDone >= 100 || lastRenderedPercent < 0 ||
            percentDone >= lastRenderedPercent + PROGRESS_STEP_PERCENT ||
            now - lastProgressUpdateMs >= PROGRESS_MIN_UPDATE_MS) {
          lastRenderedPercent = percentDone;
          lastProgressUpdateMs = now;
          requestUpdate(true);
        }
      },
      &cancelDownload);

  if (result == HttpDownloader::ABORTED) {
    // The user pressed Back, or the budget ran out. The partial file is already
    // gone; leave without an error screen, and let the next boot try again.
    LOG_INF("DLV", "Download cancelled");
    state = Finished;
    finish();
    return false;
  }

  if (result != HttpDownloader::OK) {
    LOG_ERR("DLV", "Download failed: %d", static_cast<int>(result));
    return false;
  }

  // Without this a 200-with-error-page would be saved under today's name, and
  // the exists() check above would make sure that day never retried.
  if (!looksLikeEpub(tempPath)) {
    LOG_ERR("DLV", "Not an EPUB, discarding");
    Storage.remove(tempPath.c_str());
    settle(StrId::STR_DELIVERY_NOT_A_BOOK, false);
    return false;
  }

  // Only now, with a verified book in hand, is the previous one replaced.
  Storage.remove(destPath.c_str());
  if (!Storage.rename(tempPath.c_str(), destPath.c_str())) {
    LOG_ERR("DLV", "Could not move book into place");
    Storage.remove(tempPath.c_str());
    return false;
  }

  deliveredPath = destPath;
  return true;
}

void BookDeliveryActivity::runDelivery() {
  // The clock is what names the file and decides whether anything is owed. If
  // it has never been set, this is the one chance to fix that.
  if (!DeliverySchedule::isClockValid(nowUtc()) && halClock.syncFromNTP()) {
    SETTINGS.clockHasBeenSynced = 1;
    SETTINGS.saveToFile();
  }

  const time_t now = nowUtc();
  if (!DeliverySchedule::isClockValid(now)) {
    settle(StrId::STR_DELIVERY_NO_CLOCK, false);
    return;
  }

  // Boot decided a delivery was owed while the clock may still have been
  // unset. Now that it is known, ask again before spending the radio.
  if (trigger != DeliveryTrigger::Manual &&
      !DeliverySchedule::isDue(now, APP_STATE.lastDeliveryFetch, SETTINGS.deliveryHour)) {
    settle(StrId::STR_DELIVERY_UP_TO_DATE, true);
    return;
  }

  if (!downloadTodaysBook(DeliverySchedule::fileNameFor(now))) {
    // A rejected non-EPUB settled with a more specific message; a cancelled
    // one already finished. Only an unexplained failure needs the generic one.
    if (state != Finished) {
      settle(StrId::STR_DELIVERY_FAILED, false);
    }
    return;
  }

  APP_STATE.lastDeliveryFetch = static_cast<uint32_t>(nowUtc());
  APP_STATE.saveToFile();

  // The book is on the card but the index has never seen it. Drop the index
  // rather than rebuilding here: LibraryListActivity already rebuilds whenever
  // it finds one missing, behind its own progress popup, so the full SD walk
  // lands when the library is opened instead of delaying a boot-time delivery.
  if (Storage.exists(library::libraryIndexPath())) {
    Storage.remove(library::libraryIndexPath());
    LOG_INF("DLV", "Library index dropped; it rebuilds on next open");
  }

  settle(StrId::STR_DELIVERY_DONE, true);
}

void BookDeliveryActivity::settle(const StrId message, const bool ok) {
  outcome = message;
  succeeded = ok;
  state = Finished;
  settledAtMs = millis();

  LOG_INF("DLV", "%s", I18n::getInstance().get(message));

  // A catch-up run is in the user's way on the path to the home screen: say
  // what happened only when it failed, and otherwise get out of the way.
  if (trigger == DeliveryTrigger::CatchUp && ok) {
    finish();
    return;
  }

  // Skip the result screen entirely and open the fresh book. Manual only: a
  // catch-up must not decide what the device shows after a boot.
  //
  // Reboot into it rather than pushing the reader on top of a live network
  // stack: the download leaves the WiFi driver holding its heap and the pool
  // fragmented, and the reader is the hungriest activity in the firmware. The
  // restart is the same one FontDownloadActivity and the OTA flow take, and
  // e-ink holds the last frame until the reader paints.
  if (trigger == DeliveryTrigger::Manual && ok && SETTINGS.openDeliveredBook && !deliveredPath.empty()) {
    APP_STATE.openEpubPath = deliveredPath;
    APP_STATE.saveToFile();
    silentRestartToReader();
    return;
  }

  requestUpdate();
}

void BookDeliveryActivity::loop() {
  if (state == Working) {
    // Paint "Downloading..." before blocking on the network.
    requestUpdateAndWait();
    runDelivery();
    return;
  }

  if (state != Finished) {
    return;
  }

  // A catch-up failure must not hold the device hostage on the way to the home
  // screen. A URL that 404s every morning would otherwise demand a keypress on
  // every single power-up before you could read anything.
  if (trigger == DeliveryTrigger::CatchUp && millis() - settledAtMs > FAILURE_NOTICE_MS) {
    finish();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
  }
}

void BookDeliveryActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_BOOK_DELIVERY));

  const int midY = pageHeight / 2;

  if (state == Working) {
    renderer.drawCenteredText(UI_12_FONT_ID, midY - 20, tr(STR_DELIVERY_DOWNLOADING));

    if (lastRenderedPercent >= 0) {
      char line[8];
      snprintf(line, sizeof(line), "%d%%", percentDone);
      renderer.drawCenteredText(UI_10_FONT_ID, midY + 10, line);

      const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
      GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    }

    renderer.displayBuffer();
    return;
  }

  renderer.drawCenteredText(UI_12_FONT_ID, midY, I18n::getInstance().get(outcome), true,
                            succeeded ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR);

  if (state == Finished) {
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
