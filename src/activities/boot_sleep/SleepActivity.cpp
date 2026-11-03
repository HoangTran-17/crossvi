#include "SleepActivity.h"

#include <Epub.h>
#include <Epub/SourceIdentityStore.h>
#include <Epub/converters/PngToFramebufferConverter.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalDisplay.h>
#include <HalGPIO.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <I18n.h>
#include <LunarCalendar.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub/converters/DitherUtils.h"
#include "Memory.h"
#include "RecentBooksStore.h"
#include "SleepBrief.h"
#include "SleepFrameStore.h"
#include "SleepImagePlacement.h"
#include "SleepImageSelectionStore.h"
#include "SleepImageValidation.h"
#include "activities/home/DashboardProgress.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/BookStatsLoader.h"
#include "activities/reader/DailyReadingHistory.h"
#include "activities/reader/GlobalReadingStats.h"
#include "activities/reader/ProgressFile.h"
#include "activities/reader/ProgressFileCodec.h"
#include "activities/reader/ReaderUtils.h"
#include "activities/reader/ReadingCalendarLayout.h"
#include "activities/reader/ReadingCalendarModel.h"
#include "activities/reader/ReadingCalendarRenderer.h"
#include "activities/reader/ReadingStatsPresentation.h"
#include "activities/reader/ReadingStatsUtils.h"
#include "clippings/ClippingStore.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "images/DefaultSleepScreens.h"
#include "images/Logo120.h"
#include "images/MoonIcon.h"

namespace {
// A sleep render is the last panel operation before the MCU enters deep sleep.
// Power the panel down as part of that refresh so teardown work cannot leave it
// electrically driven and darken the image after it has settled.
constexpr bool TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH = true;
// Grayscale sleep covers still need one clean base conditioning pass before
// their gray planes are written. Plain B/W sleep frames finish with the OEM
// full waveform itself, followed immediately by panel power-off.
constexpr uint8_t X3_SLEEP_CONDITION_PASSES = 1;

void prepareStrongSleepRefresh() { display.requestResync(X3_SLEEP_CONDITION_PASSES); }

void runFastSleepCleanupPass() {
  if (!display.supportsX3GhostCleanup()) {
    // X4 and X3 controllers without the validated cleanup LUT keep the stock
    // black-flash waveform as their safe equivalent.
    LOG_INF("SLW", "sleep ghost fast-clean using half fallback");
    display.displayBuffer(HalDisplay::HALF_REFRESH, false);
    return;
  }

  display.displayBuffer(HalDisplay::FAST_REFRESH, false);
  const bool cleanupStarted = display.cleanX3GhostingNow();
  LOG_INF("SLW", "sleep ghost fast-clean x3 started=%u", static_cast<unsigned>(cleanupStarted));
  if (!cleanupStarted) {
    // A pending initial resync can promote FAST to FULL, which deliberately
    // makes the X3 cleanup LUT ineligible. Fall back instead of silently
    // weakening the selected sleep treatment.
    display.displayBuffer(HalDisplay::HALF_REFRESH, false);
  }
}

void applySleepGhostingTreatment() {
  const uint8_t treatment = SETTINGS.sleepGhostingTreatment < CrossPointSettings::SLEEP_GHOSTING_TREATMENT_COUNT
                                ? SETTINGS.sleepGhostingTreatment
                                : CrossPointSettings::SLEEP_GHOST_FAST_CLEAN_FULL;
  LOG_INF("SLW", "sleep ghost treatment=%u pre-refresh begin", static_cast<unsigned>(treatment));

  switch (treatment) {
    case CrossPointSettings::SLEEP_GHOST_FAST_FULL:
      display.displayBuffer(HalDisplay::FAST_REFRESH, false);
      break;
    case CrossPointSettings::SLEEP_GHOST_FAST_TWICE_FULL:
      display.displayBuffer(HalDisplay::FAST_REFRESH, false);
      display.displayBuffer(HalDisplay::FAST_REFRESH, false);
      break;
    case CrossPointSettings::SLEEP_GHOST_FAST_CLEAN_FULL:
      runFastSleepCleanupPass();
      break;
    case CrossPointSettings::SLEEP_GHOST_FAST_CLEAN_TWICE_FULL:
      runFastSleepCleanupPass();
      runFastSleepCleanupPass();
      break;
    case CrossPointSettings::SLEEP_GHOST_HALF_FULL:
      display.displayBuffer(HalDisplay::HALF_REFRESH, false);
      break;
    case CrossPointSettings::SLEEP_GHOST_HALF_TWICE_FULL:
      display.displayBuffer(HalDisplay::HALF_REFRESH, false);
      display.displayBuffer(HalDisplay::HALF_REFRESH, false);
      break;
    case CrossPointSettings::SLEEP_GHOST_FULL_TWICE:
      display.displayBuffer(HalDisplay::FULL_REFRESH, false);
      break;
    case CrossPointSettings::SLEEP_GHOST_FULL_THREE_TIMES:
      display.displayBuffer(HalDisplay::FULL_REFRESH, false);
      display.displayBuffer(HalDisplay::FULL_REFRESH, false);
      break;
    case CrossPointSettings::SLEEP_GHOST_FULL_ONLY:
    default:
      break;
  }
}

void displayStrongSleepFrame() {
  const unsigned long startedAt = millis();
  applySleepGhostingTreatment();
  LOG_INF("SLW", "sleep-frame terminal full refresh begin turn_off=1");
  // FULL_REFRESH remains mandatory. On X3 the OEM full waveform is the final
  // physical panel update; the driver powers the panel off as soon as it ends.
  display.displayBuffer(HalDisplay::FULL_REFRESH, TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
  LOG_INF("SLW", "sleep-frame full refresh complete elapsed_ms=%lu", millis() - startedAt);
}

void drawCenteredInRect(const GfxRenderer& renderer, const int fontId, const Rect& rect, const int y, const char* text,
                        const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const int width = renderer.getTextWidth(fontId, text, style);
  renderer.drawText(fontId, rect.x + std::max(0, (rect.width - width) / 2), y, text, true, style);
}

void drawCalendarMetric(const GfxRenderer& renderer, const Rect& rect, const std::string& value, const StrId label,
                        const int valueFontId = UI_10_FONT_ID) {
  const int valueHeight = renderer.getLineHeight(valueFontId);
  const int labelHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int top = rect.y + std::max(0, (rect.height - valueHeight - labelHeight - 4) / 2);
  const std::string shownValue =
      renderer.truncatedText(valueFontId, value.c_str(), rect.width - 8, EpdFontFamily::BOLD);
  const std::string shownLabel = renderer.truncatedText(SMALL_FONT_ID, I18N.get(label), rect.width - 8);
  drawCenteredInRect(renderer, valueFontId, rect, top, shownValue.c_str(), EpdFontFamily::BOLD);
  drawCenteredInRect(renderer, SMALL_FONT_ID, rect, top + valueHeight + 4, shownLabel.c_str());
}

void drawCalendarWeekdays(const GfxRenderer& renderer, const ReadingCalendarGridLayout& layout) {
  constexpr std::array<StrId, ReadingCalendarGridLayout::COLUMNS> labels = {
      StrId::STR_STATS_MON, StrId::STR_STATS_TUE, StrId::STR_STATS_WED, StrId::STR_STATS_THU,
      StrId::STR_STATS_FRI, StrId::STR_STATS_SAT, StrId::STR_STATS_SUN};
  for (size_t index = 0; index < labels.size(); ++index) {
    const Rect column{layout.grid.x + static_cast<int>(index) * (layout.cellSize + ReadingCalendarGridLayout::CELL_GAP),
                      layout.weekdays.y, layout.cellSize, layout.weekdays.height};
    drawCenteredInRect(renderer, SMALL_FONT_ID, column, column.y, I18N.get(labels[index]), EpdFontFamily::BOLD);
  }
}

// The sleep digit fonts carry no descenders, so a day number's ink runs from
// the top of the digits down to the baseline.
int digitInkHeight(const GfxRenderer& renderer, const int fontId) {
  const auto font = renderer.getFontMap().find(fontId);
  const EpdGlyph* zero = font == renderer.getFontMap().end() ? nullptr : font->second.getGlyph('0');
  return zero ? zero->top : renderer.getFontAscenderSize(fontId);
}

// Draws `digits` with its ink top at `inkTop`.
void drawDigits(const GfxRenderer& renderer, const int fontId, const int x, const int inkTop, const char* digits) {
  renderer.drawText(fontId, x, inkTop - renderer.getFontAscenderSize(fontId) + digitInkHeight(renderer, fontId),
                    digits);
}

// A cell carries the solar day at the top and the lunar day at the bottom.
int minLunarCellSize(const GfxRenderer& renderer) { return renderer.getLineHeight(SMALL_FONT_ID) * 2 + 10; }

// Lunar days only advance by one inside a month, so the astronomical
// conversion runs again only where a new lunar month may begin.
void drawLunarDayNumbers(const GfxRenderer& renderer, const ReadingCalendarGridLayout& layout,
                         const ReadingCalendarModel& model) {
  if (layout.cellSize < minLunarCellSize(renderer)) return;
  const int lineHeight = renderer.getLineHeight(SMALL_FONT_ID);
  lunar::DayInfo info{};
  bool known = false;
  for (size_t index = 0; index < ReadingCalendarGridLayout::CELL_COUNT; ++index) {
    const ReadingCalendarCell cell = model.cellAt(index);
    if (!cell.date.isValid()) {
      known = false;
      continue;
    }
    if (known && info.lunarDay < 29) {
      ++info.lunarDay;
    } else {
      known = lunar::computeDay(cell.date.year, cell.date.month, cell.date.day, info);
      if (!known) continue;
    }
    char label[8];
    if (info.lunarDay == 1) {
      snprintf(label, sizeof(label), "1/%u", static_cast<unsigned>(info.lunarMonth));
    } else {
      snprintf(label, sizeof(label), "%u", static_cast<unsigned>(info.lunarDay));
    }
    const ReadingHeatmapLevel level =
        cell.exactDuration ? readingHeatmapLevel(cell.readingSeconds) : ReadingHeatmapLevel::None;
    const bool blackText = level != ReadingHeatmapLevel::Minutes120 && level != ReadingHeatmapLevel::Minutes240;
    const Rect box = layout.cell(index);
    const int width = renderer.getTextWidth(SMALL_FONT_ID, label);
    renderer.drawText(SMALL_FONT_ID, box.x + box.width - width - 4, box.y + box.height - lineHeight - 6, label,
                      blackText);
  }
}

// Draws the first candidate that fits `width`, trying each font in order;
// the last candidate is truncated when nothing fits.
void drawFittedLine(const GfxRenderer& renderer, const int x, const int y, const int width,
                    std::initializer_list<const char*> candidates, std::initializer_list<int> fonts,
                    const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  for (const char* text : candidates) {
    for (const int fontId : fonts) {
      if (renderer.getTextWidth(fontId, text, style) <= width) {
        renderer.drawText(fontId, x, y, text, true, style);
        return;
      }
    }
  }
  const int fontId = *(fonts.end() - 1);
  const std::string shown = renderer.truncatedText(fontId, *(candidates.end() - 1), width, style);
  renderer.drawText(fontId, x, y, shown.c_str(), true, style);
}

// Centered variant of drawFittedLine for a single text.
void drawCenteredFittedLine(const GfxRenderer& renderer, const Rect& rect, const int y, const char* text,
                            std::initializer_list<int> fonts,
                            const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  for (const int fontId : fonts) {
    if (renderer.getTextWidth(fontId, text, style) <= rect.width) {
      drawCenteredInRect(renderer, fontId, rect, y, text, style);
      return;
    }
  }
  const int fontId = *(fonts.end() - 1);
  const std::string shown = renderer.truncatedText(fontId, text, rect.width, style);
  drawCenteredInRect(renderer, fontId, rect, y, shown.c_str(), style);
}

struct SleepBookSummary {
  std::string title;
  std::string author;
  std::string chapter;
  std::string readingTime;
  std::string sessions;
  std::string pagesTurned;
  ReadingStatsMetric progress = ReadingStatsMetric::unavailable();
  bool progressBelowOnePercent = false;
  bool available = false;
};

std::string bookTitleFromPath(const std::string& path) {
  const size_t separator = path.find_last_of('/');
  std::string title = separator == std::string::npos ? path : path.substr(separator + 1);
  const size_t extension = title.find_last_of('.');
  if (extension != std::string::npos && extension > 0) title.resize(extension);
  return title;
}

std::string formatSleepDuration(const uint32_t seconds, const bool estimated) {
  char value[28];
  const char* prefix = estimated ? "~" : "";
  if (seconds == 0) {
    snprintf(value, sizeof(value), "%s0m", prefix);
  } else if (seconds < 60) {
    snprintf(value, sizeof(value), "%s<1m", prefix);
  } else {
    const uint32_t minutes = seconds / 60;
    if (minutes < 60) {
      snprintf(value, sizeof(value), "%s%lum", prefix, static_cast<unsigned long>(minutes));
    } else {
      const uint32_t hours = minutes / 60;
      const uint32_t remainder = minutes % 60;
      if (hours < 1000 && remainder > 0) {
        snprintf(value, sizeof(value), "%s%luh %lum", prefix, static_cast<unsigned long>(hours),
                 static_cast<unsigned long>(remainder));
      } else {
        snprintf(value, sizeof(value), "%s%luh", prefix, static_cast<unsigned long>(hours));
      }
    }
  }
  return value;
}

std::string formatSleepMetric(const ReadingStatsMetric& metric, const bool duration) {
  switch (metric.state) {
    case ReadingStatsMetricState::NotApplicable:
      return tr(STR_STATS_NOT_APPLICABLE);
    case ReadingStatsMetricState::NoData:
      return tr(STR_STATS_NO_DATA);
    case ReadingStatsMetricState::Unavailable:
      return tr(STR_STATS_UNAVAILABLE);
    case ReadingStatsMetricState::Known:
    case ReadingStatsMetricState::Estimated:
      if (duration) return formatSleepDuration(metric.value, metric.state == ReadingStatsMetricState::Estimated);
      return std::string(metric.state == ReadingStatsMetricState::Estimated ? "~" : "") + std::to_string(metric.value);
  }
  return tr(STR_STATS_UNAVAILABLE);
}

std::string formatSleepProgress(const SleepBookSummary& summary) {
  if (summary.progress.state != ReadingStatsMetricState::Known &&
      summary.progress.state != ReadingStatsMetricState::Estimated) {
    return formatSleepMetric(summary.progress, false);
  }
  const char* prefix = summary.progress.state == ReadingStatsMetricState::Estimated ? "~" : "";
  if (summary.progressBelowOnePercent) return std::string(prefix) + "<1%";
  return std::string(prefix) + std::to_string(std::min<uint32_t>(summary.progress.value, 100)) + "%";
}

void setSleepProgress(SleepBookSummary& summary, const uint8_t percent, const bool estimated = false,
                      const bool belowOnePercent = false) {
  summary.progress = estimated ? ReadingStatsMetric::estimated(percent) : ReadingStatsMetric::known(percent);
  summary.progressBelowOnePercent = belowOnePercent;
}

bool hasTrustedSourceIdentity(const std::string& cachePath, const ZipFile::SourceIdentity& current) {
  ZipFile::SourceIdentity stored;
  const SourceIdentityStore::LoadStatus status = SourceIdentityStore::load(cachePath, stored);
  return (status == SourceIdentityStore::LoadStatus::Primary || status == SourceIdentityStore::LoadStatus::Backup ||
          status == SourceIdentityStore::LoadStatus::Temp) &&
         stored == current;
}

void loadEpubSleepPosition(const RecentBook& recent, SleepBookSummary& summary) {
  Epub book(recent.path, "/.crosspoint");
  if (book.inspectCache() != BookMetadataCache::LoadStatus::Loaded) return;

  std::array<uint8_t, ProgressFile::EPUB_CONTENT_ANCHORED_PROGRESS_SIZE> bytes{};
  const ProgressFile::EpubBounds bounds{static_cast<uint32_t>(book.getSpineItemsCount())};
  const ProgressFile::CandidateValidator validator{ProgressFile::validateEpubBounds, &bounds};
  const ProgressFile::LoadResult loaded =
      ProgressFile::loadEpub(book.getCachePath(), bytes.data(), bytes.size(), validator);
  if (!loaded) {
    if (loaded.source == ProgressFile::LoadSource::Missing) summary.progress = ReadingStatsMetric::noData();
    return;
  }

  const uint16_t spineIndex = static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8;
  const int tocIndex = book.getTocIndexForSpineIndex(spineIndex);
  if (tocIndex >= 0) summary.chapter = book.getTocItem(tocIndex).title;

  DashboardProgress::Position position;
  if (!DashboardProgress::decode(bytes.data(), loaded.size, position)) return;
  const float chapterProgress = static_cast<float>(position.pageNumber + 1U) / position.pageCount;
  float bookProgress = 0.0F;
  uint8_t percent = 0;
  if (book.calculateProgressChecked(position.spineIndex, chapterProgress, bookProgress) &&
      DashboardProgress::toPercent(bookProgress, percent)) {
    setSleepProgress(summary, percent, false, bookProgress > 0.0F && percent == 0);
  }
}

void loadXtcSleepPosition(const RecentBook& recent, SleepBookSummary& summary) {
  Xtc book(recent.path, "/.crosspoint");
  ZipFile::SourceIdentity identity;
  if (!book.load() || !book.getSourceIdentity(identity) || !hasTrustedSourceIdentity(book.getCachePath(), identity)) {
    return;
  }

  uint8_t bytes[4]{};
  const ProgressFile::PageBounds bounds{book.getPageCount()};
  const ProgressFile::CandidateValidator validator{ProgressFile::validatePageBounds, &bounds};
  const ProgressFile::LoadResult loaded = ProgressFile::loadPage(book.getCachePath(), bytes, sizeof(bytes), validator);
  if (!loaded) {
    if (loaded.source == ProgressFile::LoadSource::Missing) summary.progress = ReadingStatsMetric::noData();
    return;
  }

  const uint32_t page = ProgressFileCodec::decodeU32(bytes);
  setSleepProgress(summary, book.calculateProgress(page));
  if (!book.hasChapters()) return;
  const auto& chapters = book.getChapters();
  const auto chapter = std::find_if(chapters.begin(), chapters.end(), [page](const xtc::ChapterInfo& entry) {
    return page >= entry.startPage && page <= entry.endPage;
  });
  if (chapter != chapters.end()) summary.chapter = chapter->name;
}

void loadTxtSleepPosition(const RecentBook& recent, SleepBookSummary& summary) {
  Txt book(recent.path, "/.crosspoint");
  ZipFile::SourceIdentity identity;
  if (!book.load() || !book.getSourceIdentity(identity) || !hasTrustedSourceIdentity(book.getCachePath(), identity) ||
      book.getFileSize() == 0 || book.getFileSize() > UINT32_MAX) {
    return;
  }

  uint8_t bytes[ProgressFileCodec::TXT_V2_SIZE]{};
  const ProgressFile::TxtBounds bounds{static_cast<uint32_t>(book.getFileSize()), 0};
  const ProgressFile::CandidateValidator validator{ProgressFile::validateTxtBounds, &bounds};
  const ProgressFile::LoadResult loaded = ProgressFile::loadTxt(book.getCachePath(), bytes, sizeof(bytes), validator);
  if (!loaded) {
    if (loaded.source == ProgressFile::LoadSource::Missing) summary.progress = ReadingStatsMetric::noData();
    return;
  }

  uint32_t byteOffset = 0;
  if (ProgressFileCodec::decodeTxt(bytes, loaded.size, byteOffset) != ProgressFileCodec::TxtDecodeStatus::Ok) return;
  const uint32_t percent = static_cast<uint32_t>(static_cast<uint64_t>(byteOffset) * 100U / book.getFileSize());
  setSleepProgress(summary, static_cast<uint8_t>(std::min<uint32_t>(percent, 100)), true,
                   byteOffset > 0 && percent == 0);
}

void loadSleepBookPosition(const RecentBook& recent, SleepBookSummary& summary) {
  if (FsHelpers::hasEpubExtension(recent.path)) {
    loadEpubSleepPosition(recent, summary);
  } else if (FsHelpers::hasXtcExtension(recent.path)) {
    loadXtcSleepPosition(recent, summary);
  } else if (FsHelpers::hasTxtExtension(recent.path) || FsHelpers::hasMarkdownExtension(recent.path)) {
    loadTxtSleepPosition(recent, summary);
  }
}

SleepBookSummary loadSleepBookSummary() {
  SleepBookSummary summary;
  const std::string& path = APP_STATE.openEpubPath;
  if (path.empty() || !Storage.exists(path.c_str())) return summary;

  RecentBook recent{path, bookTitleFromPath(path), "", ""};
  const auto& books = RECENT_BOOKS.getBooks();
  const auto found =
      std::find_if(books.begin(), books.end(), [&path](const RecentBook& book) { return book.path == path; });
  if (found != books.end()) recent = *found;
  if (recent.title.empty()) recent.title = bookTitleFromPath(path);

  summary.title = recent.title;
  summary.author = recent.author.empty() ? tr(STR_STATS_NO_DATA) : recent.author;
  summary.available = !summary.title.empty();
  if (!summary.available) return summary;

  loadSleepBookPosition(recent, summary);

  BookReadingStats stats;
  if (!loadTrustedBookReadingStats(recent, stats)) {
    summary.readingTime = tr(STR_STATS_UNAVAILABLE);
    summary.sessions = tr(STR_STATS_UNAVAILABLE);
    summary.pagesTurned = tr(STR_STATS_UNAVAILABLE);
    return summary;
  }
  if (stats.isCompleted && !stats.completionUnavailable) setSleepProgress(summary, 100);
  summary.readingTime =
      formatSleepMetric(stats.readingTimeUnavailable ? ReadingStatsMetric::noData()
                                                     : ReadingStatsMetric::known(stats.totalReadingSeconds),
                        true);
  summary.sessions = formatSleepMetric(
      stats.sessionsUnavailable ? ReadingStatsMetric::noData() : ReadingStatsMetric::known(stats.sessionCount), false);
  summary.pagesTurned = formatSleepMetric(
      stats.pageTurnsUnavailable ? ReadingStatsMetric::noData() : ReadingStatsMetric::known(stats.totalPagesTurned),
      false);
  return summary;
}

Rect drawSleepBookStatsOverlay(const GfxRenderer& renderer, const SleepBookSummary& summary) {
  if (!summary.available) return Rect{};

  constexpr int cardMargin = 16;
  constexpr int topPadding = 16;
  constexpr int leftPadding = 18;
  constexpr int rightPadding = 14;
  constexpr int titleAuthorGap = 8;
  constexpr int authorChapterGap = 8;
  constexpr int detailsGap = 10;
  constexpr int progressBarHeight = 10;
  constexpr int progressStatsGap = 12;
  constexpr int statsHeight = 76;
  constexpr int bottomPadding = 11;
  const int cardWidth = renderer.getScreenWidth() - cardMargin * 2;
  const int contentWidth = cardWidth - leftPadding - rightPadding;
  const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int authorLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int detailsLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const auto titleLines =
      renderer.wrappedText(UI_12_FONT_ID, summary.title.c_str(), contentWidth, 2, EpdFontFamily::BOLD);
  const int titleBlockHeight = titleLineHeight * static_cast<int>(titleLines.size());
  const int authorYInCard = topPadding + titleBlockHeight + titleAuthorGap;
  const int chapterYInCard = authorYInCard + authorLineHeight + authorChapterGap;
  const int progressYInCard = summary.chapter.empty() ? authorYInCard + authorLineHeight + detailsGap
                                                      : chapterYInCard + detailsLineHeight + detailsGap;
  const int progressBarYInCard = progressYInCard + detailsLineHeight + 4;
  const int statsTopInCard = progressBarYInCard + progressBarHeight + progressStatsGap;
  const int cardHeight = statsTopInCard + 1 + statsHeight + bottomPadding;
  const Rect card{cardMargin, renderer.getScreenHeight() - cardMargin - cardHeight, cardWidth, cardHeight};

  renderer.fillRect(card.x, card.y, card.width, card.height, false);
  renderer.drawRect(card.x, card.y, card.width, card.height, 2, true);
  renderer.fillRect(card.x + 2, card.y + 2, 5, card.height - 4, true);

  const int contentX = card.x + leftPadding;
  int y = card.y + topPadding;

  for (const std::string& line : titleLines) {
    renderer.drawText(UI_12_FONT_ID, contentX, y, line.c_str(), true, EpdFontFamily::BOLD);
    y += titleLineHeight;
  }
  y = card.y + authorYInCard;
  const std::string author = renderer.truncatedText(UI_10_FONT_ID, summary.author.c_str(), contentWidth);
  renderer.drawText(UI_10_FONT_ID, contentX, y, author.c_str());

  if (!summary.chapter.empty()) {
    const std::string chapterLabel = std::string(tr(STR_CHAPTER_PREFIX)) + summary.chapter;
    const std::string chapter = renderer.truncatedText(UI_10_FONT_ID, chapterLabel.c_str(), contentWidth);
    renderer.drawText(UI_10_FONT_ID, contentX, card.y + chapterYInCard, chapter.c_str());
  }

  const std::string progressValue = formatSleepProgress(summary);
  renderer.drawText(UI_10_FONT_ID, contentX, card.y + progressYInCard, tr(STR_STATS_PROGRESS), true,
                    EpdFontFamily::BOLD);
  const int progressValueWidth = renderer.getTextWidth(UI_10_FONT_ID, progressValue.c_str());
  renderer.drawText(UI_10_FONT_ID, contentX + std::max(0, contentWidth - progressValueWidth), card.y + progressYInCard,
                    progressValue.c_str());
  const int progressBarY = card.y + progressBarYInCard;
  renderer.drawRoundedRect(contentX, progressBarY, contentWidth, progressBarHeight, 1, 3, true);
  if (summary.progress.state == ReadingStatsMetricState::Known ||
      summary.progress.state == ReadingStatsMetricState::Estimated) {
    const int fillWidth = DashboardProgress::fillWidth(contentWidth, static_cast<uint8_t>(summary.progress.value));
    if (fillWidth > 0) {
      renderer.fillRoundedRect(contentX + 2, progressBarY + 2, fillWidth, progressBarHeight - 4, 1, Color::Black);
    }
  }

  const int statsTop = card.y + statsTopInCard;
  renderer.drawLine(contentX, statsTop, contentX + contentWidth - 1, statsTop);
  const Rect stats{contentX, statsTop + 1, contentWidth, statsHeight};
  const int firstWidth = stats.width * 40 / 100;
  const int secondWidth = stats.width * 25 / 100;
  renderer.drawLine(stats.x + firstWidth, stats.y + 8, stats.x + firstWidth, stats.y + stats.height - 8);
  renderer.drawLine(stats.x + firstWidth + secondWidth, stats.y + 8, stats.x + firstWidth + secondWidth,
                    stats.y + stats.height - 8);
  drawCalendarMetric(renderer, Rect{stats.x, stats.y, firstWidth, stats.height}, summary.readingTime,
                     StrId::STR_STATS_READING_TIME, UI_12_FONT_ID);
  drawCalendarMetric(renderer, Rect{stats.x + firstWidth, stats.y, secondWidth, stats.height}, summary.sessions,
                     StrId::STR_STATS_SESSIONS);
  drawCalendarMetric(
      renderer, Rect{stats.x + firstWidth + secondWidth, stats.y, stats.width - firstWidth - secondWidth, stats.height},
      summary.pagesTurned, StrId::STR_STATS_PAGES_TURNED);
  return card;
}

Rect drawSleepBookStatsOverlay(const GfxRenderer& renderer) {
  return drawSleepBookStatsOverlay(renderer, loadSleepBookSummary());
}

struct PopupSnapshot {
  Rect rect{};
  std::unique_ptr<uint8_t[]> bytes;
  size_t size = 0;
};

Rect enteringSleepPopupOuterRect(const GfxRenderer& renderer, const char* message) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const EpdFontFamily::Style style = metrics.popupTextBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;
  const int y = static_cast<int>(renderer.getScreenHeight() * metrics.popupTopOffsetRatio);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, style);
  const int textHeight = renderer.getLineHeight(UI_12_FONT_ID);
  const int w = textWidth + metrics.popupMarginX * 2;
  const int h = textHeight + metrics.popupMarginY * 2;
  const int x = (renderer.getScreenWidth() - w) / 2;
  return Rect{x - metrics.popupFrameThickness, y - metrics.popupFrameThickness, w + metrics.popupFrameThickness * 2,
              h + metrics.popupFrameThickness * 2};
}

PopupSnapshot capturePopupSnapshot(const GfxRenderer& renderer, const char* message) {
  PopupSnapshot snapshot;
  snapshot.rect = enteringSleepPopupOuterRect(renderer, message);
  snapshot.size =
      renderer.getRegionByteSize(snapshot.rect.x, snapshot.rect.y, snapshot.rect.width, snapshot.rect.height);
  if (snapshot.size == 0) return snapshot;
  snapshot.bytes = makeUniqueNoThrow<uint8_t[]>(snapshot.size);
  if (!snapshot.bytes) {
    snapshot.size = 0;
    return snapshot;
  }
  if (!renderer.copyRegionToBuffer(snapshot.rect.x, snapshot.rect.y, snapshot.rect.width, snapshot.rect.height,
                                   snapshot.bytes.get(), snapshot.size)) {
    snapshot.bytes.reset();
    snapshot.size = 0;
  }
  return snapshot;
}

void restorePopupSnapshot(const GfxRenderer& renderer, const PopupSnapshot& snapshot) {
  if (!snapshot.bytes || snapshot.size == 0) return;
  renderer.copyBufferToRegion(snapshot.rect.x, snapshot.rect.y, snapshot.rect.width, snapshot.rect.height,
                              snapshot.bytes.get(), snapshot.size);
}

void showEnteringSleepPopup(const GfxRenderer& renderer, const char* message, const bool transparent) {
  if (!transparent) {
    GUI.drawPopup(renderer, message);
    return;
  }
  const PopupSnapshot snapshot = capturePopupSnapshot(renderer, message);
  if (!snapshot.bytes || snapshot.size == 0) {
    LOG_ERR("SLP", "Skipping transparent sleep popup: snapshot unavailable");
    return;
  }
  GUI.drawPopup(renderer, message);
  restorePopupSnapshot(renderer, snapshot);
}

void drawMoonOnCurrentFrame(const GfxRenderer& renderer) {
  const int pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
}

SleepImagePlacement placeSleepImage(const GfxRenderer& renderer, const int sourceWidth, const int sourceHeight,
                                    const bool applyUserTransform = true) {
  return calculateSleepImagePlacement(renderer.getScreenWidth(), renderer.getScreenHeight(), sourceWidth, sourceHeight,
                                      applyUserTransform ? SETTINGS.sleepScreenImageZoom : SLEEP_IMAGE_DEFAULT_ZOOM,
                                      applyUserTransform ? SETTINGS.sleepScreenImageOffsetX : 0,
                                      applyUserTransform ? SETTINGS.sleepScreenImageOffsetY : 0);
}

SleepImagePlacement placeSleepImage(const GfxRenderer& renderer, const int sourceWidth, const int sourceHeight,
                                    const SleepImageSelectionStore::ImageTransform& transform) {
  return calculateSleepImagePlacement(renderer.getScreenWidth(), renderer.getScreenHeight(), sourceWidth, sourceHeight,
                                      transform.zoom, transform.offsetX, transform.offsetY);
}

bool alphaCoveragePasses(const uint8_t alpha, const int x, const int y) {
  if (alpha == 0) return false;
  if (alpha == 255) return true;
  return alpha > static_cast<uint8_t>(bayer4x4[y & 3][x & 3] * 16U);
}

using Bmp32OverlayHeader = SleepImageValidation::Bmp32Header;
using Bmp32HeaderStatus = SleepImageValidation::Bmp32HeaderStatus;

bool renderBmp32Overlay(HalFile& file, const Bmp32OverlayHeader& header, GfxRenderer& renderer,
                        const SleepImageSelectionStore::ImageTransform& transform) {
  const SleepImagePlacement placement = placeSleepImage(renderer, header.width, header.height, transform);
  if (placement.width <= 0 || placement.height <= 0) return false;
  auto row = makeUniqueNoThrow<uint8_t[]>(header.rowBytes);
  if (!row) return false;

  const int firstVisibleY = std::max(0, -placement.y);
  const int lastVisibleY = std::min(placement.height, renderer.getScreenHeight() - placement.y);
  const int firstVisibleX = std::max(0, -placement.x);
  const int lastVisibleX = std::min(placement.width, renderer.getScreenWidth() - placement.x);
  if (firstVisibleX >= lastVisibleX || firstVisibleY >= lastVisibleY) return true;

  for (int outY = firstVisibleY; outY < lastVisibleY; ++outY) {
    const int sourceY = std::min(header.height - 1, static_cast<int>(std::floor(outY / placement.scale)));
    const int fileRow = header.topDown ? sourceY : header.height - 1 - sourceY;
    const uint64_t rowOffset = header.pixelOffset + static_cast<uint64_t>(fileRow) * header.rowBytes;
    if (!file.seek64(rowOffset) || file.read(row.get(), header.rowBytes) != static_cast<int>(header.rowBytes)) {
      return false;
    }
    const int screenY = placement.y + outY;
    for (int outX = firstVisibleX; outX < lastVisibleX; ++outX) {
      const int sourceX = std::min(header.width - 1, static_cast<int>(std::floor(outX / placement.scale)));
      const uint8_t* pixel = row.get() + static_cast<size_t>(sourceX) * 4U;
      const uint8_t alpha = pixel[3];
      const int screenX = placement.x + outX;
      if (!alphaCoveragePasses(alpha, screenX, screenY)) continue;
      const uint8_t gray = static_cast<uint8_t>((pixel[2] * 77U + pixel[1] * 150U + pixel[0] * 29U) >> 8U);
      renderer.drawPixel(screenX, screenY, applyBayerDither1Bit(gray, screenX, screenY));
    }
  }
  return true;
}

bool validateBmpOverlay(const std::string& path) { return SleepImageValidation::overlayBmp(path); }

bool isOverlayImageName(const std::string& filename) {
  return !filename.empty() && filename[0] != '.' && filename.size() <= 256 &&
         (FsHelpers::hasBmpExtension(filename) || FsHelpers::hasPngExtension(filename));
}

struct OverlayCandidate {
  std::string path;
  uint16_t index = 0;
  bool valid = false;
};

constexpr uint16_t MAX_SLEEP_DIRECTORY_ENTRIES = 4096;
constexpr uint8_t SLEEP_DIRECTORY_YIELD_INTERVAL = 16;

template <typename Validator>
OverlayCandidate directoryImageCandidate(const char* directoryPath, Validator&& validator) {
  HalFile dir = Storage.open(directoryPath);
  if (!dir || !dir.isDirectory()) return {};

  OverlayCandidate any;
  OverlayCandidate fresh;
  uint16_t entries = 0;
  uint16_t validCount = 0;
  uint16_t freshCount = 0;
  char name[257];
  for (HalFile file = dir.openNextFile(); file && entries < MAX_SLEEP_DIRECTORY_ENTRIES; file = dir.openNextFile()) {
    ++entries;
    if (entries % SLEEP_DIRECTORY_YIELD_INTERVAL == 0) yield();
    if (file.isDirectory()) {
      file.close();
      continue;
    }
    const size_t length = file.getName(name, sizeof(name));
    file.close();
    if (length == 0 || length >= sizeof(name)) continue;
    const std::string filename(name);
    if (!isOverlayImageName(filename)) continue;
    const std::string path = std::string(directoryPath) + "/" + filename;
    if (!validator(path)) continue;

    const uint16_t index = validCount++;
    if (random(static_cast<long>(validCount)) == 0) any = {path, index, true};
    if (!APP_STATE.isRecentSleep(index, APP_STATE.recentSleepFill)) {
      ++freshCount;
      if (random(static_cast<long>(freshCount)) == 0) fresh = {path, index, true};
    }
  }
  dir.close();

  OverlayCandidate selected = fresh.valid ? std::move(fresh) : std::move(any);
  if (selected.valid) {
    APP_STATE.pushRecentSleep(selected.index);
    APP_STATE.saveToFile();
  }
  return selected;
}

size_t selectCatalogCandidate(const std::vector<SleepImageSelectionStore::ImageEntry>& candidates) {
  size_t selected = candidates.size();
  size_t eligible = 0;
  for (size_t index = 0; index < candidates.size(); ++index) {
    const auto& candidate = candidates[index];
    if (APP_STATE.isRecentSleep(candidate.id, APP_STATE.recentSleepFill)) continue;
    ++eligible;
    if (random(static_cast<long>(eligible)) == 0) selected = index;
  }
  if (selected < candidates.size()) return selected;
  return candidates.empty() ? 0 : static_cast<size_t>(random(static_cast<long>(candidates.size())));
}

bool renderPngOverlay(const std::string& path, GfxRenderer& renderer,
                      const SleepImageSelectionStore::ImageTransform& transform) {
  ImageDimensions dimensions{};
  if (!PngToFramebufferConverter::getSupportedDimensionsStatic(path, dimensions)) return false;
  const SleepImagePlacement placement = placeSleepImage(renderer, dimensions.width, dimensions.height, transform);
  if (placement.width <= 0 || placement.height <= 0) return false;

  RenderConfig config{};
  config.x = placement.x;
  config.y = placement.y;
  config.maxWidth = placement.width;
  config.maxHeight = placement.height;
  config.useDithering = true;
  config.useExactDimensions = true;
  config.preserveAlpha = true;
  config.writeWhiteInBw = true;
  PngToFramebufferConverter converter;
  return converter.decodeToFramebuffer(path, renderer, config);
}

bool renderBitmapWhiteKeyOverlay(Bitmap& bitmap, GfxRenderer& renderer,
                                 const SleepImageSelectionStore::ImageTransform& transform) {
  const SleepImagePlacement placement = placeSleepImage(renderer, bitmap.getWidth(), bitmap.getHeight(), transform);
  if (placement.width <= 0 || placement.height <= 0) return false;

  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  auto outputRow = makeUniqueNoThrow<uint8_t[]>(outputRowSize);
  auto rowBytes = makeUniqueNoThrow<uint8_t[]>(bitmap.getRowBytes());
  if (!outputRow || !rowBytes) return false;

  for (int bmpY = 0; bmpY < bitmap.getHeight(); ++bmpY) {
    if (bitmap.readNextRow(outputRow.get(), rowBytes.get()) != BmpReaderError::Ok) return false;
    const int sourceY = bitmap.isTopDown() ? bmpY : bitmap.getHeight() - 1 - bmpY;
    const int firstY =
        placement.y + static_cast<int>(static_cast<int64_t>(sourceY) * placement.height / bitmap.getHeight());
    const int endY =
        placement.y + static_cast<int>(static_cast<int64_t>(sourceY + 1) * placement.height / bitmap.getHeight());
    if (firstY >= endY) continue;

    for (int bmpX = 0; bmpX < bitmap.getWidth(); ++bmpX) {
      const uint8_t value = (outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8))) & 0x03;
      if (value >= 3) continue;
      const int firstX =
          placement.x + static_cast<int>(static_cast<int64_t>(bmpX) * placement.width / bitmap.getWidth());
      const int endX =
          placement.x + static_cast<int>(static_cast<int64_t>(bmpX + 1) * placement.width / bitmap.getWidth());
      if (firstX >= endX) continue;
      const int clippedX = std::max(0, firstX);
      const int clippedY = std::max(0, firstY);
      const int clippedRight = std::min(renderer.getScreenWidth(), endX);
      const int clippedBottom = std::min(renderer.getScreenHeight(), endY);
      if (clippedX < clippedRight && clippedY < clippedBottom) {
        renderer.fillRect(clippedX, clippedY, clippedRight - clippedX, clippedBottom - clippedY);
      }
    }
  }
  return true;
}

bool renderBmpOverlay(const std::string& path, GfxRenderer& renderer,
                      const SleepImageSelectionStore::ImageTransform& transform) {
  HalFile file;
  if (!Storage.openFileForRead("SLP", path, file)) return false;

  Bmp32OverlayHeader alphaHeader;
  const Bmp32HeaderStatus alphaStatus = SleepImageValidation::readBmp32Header(file, alphaHeader);
  if (alphaStatus == Bmp32HeaderStatus::Valid) {
    const bool rendered = renderBmp32Overlay(file, alphaHeader, renderer, transform);
    file.close();
    return rendered;
  }
  if (alphaStatus == Bmp32HeaderStatus::Invalid) return file.close() && false;

  if (!file.seek(0)) return file.close() && false;
  Bitmap bitmap(file, true);
  if (bitmap.parseHeaders() != BmpReaderError::Ok) return file.close() && false;
  const bool rendered = renderBitmapWhiteKeyOverlay(bitmap, renderer, transform);
  file.close();
  return rendered;
}

bool renderOverlayImage(const std::string& path, GfxRenderer& renderer,
                        const SleepImageSelectionStore::ImageTransform& transform) {
  if (FsHelpers::hasPngExtension(path)) return renderPngOverlay(path, renderer, transform);
  if (FsHelpers::hasBmpExtension(path)) return renderBmpOverlay(path, renderer, transform);
  return false;
}
}  // namespace

void SleepActivity::onEnter() {
  Activity::onEnter();
  wakeFrameReplayable_ = true;

  const bool renderQuickResume =
      SETTINGS.quickResumeSleepScreen == CrossPointSettings::QUICK_RESUME_SLEEP_SCREEN::QUICK_RESUME_AFTER_TIMEOUT;
  LOG_INF("SLW", "sleep-activity enter screen=%u quick_resume=%u from_reader=%u",
          static_cast<unsigned>(SETTINGS.sleepScreen), static_cast<unsigned>(renderQuickResume),
          static_cast<unsigned>(APP_STATE.lastSleepFromReader));

  if (renderQuickResume) {
    return renderLastScreenSleepScreen();
  }

  // CUSTOM is the persisted legacy alias for the catalog-backed overlay mode.
  // CUSTOM_STATS intentionally keeps the old opaque image + stats renderer.
  const bool renderTransparent = SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM ||
                                 SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::TRANSPARENT_CUSTOM;
  const bool legacyCustomImageRequired =
      SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM_STATS ||
      (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM && !APP_STATE.lastSleepFromReader);
  if (!renderTransparent && legacyCustomImageRequired && !SleepImageSelectionStore::recover()) {
    LOG_ERR("SLP", "Sleep image selection recovery failed; using default screen");
    return renderDefaultSleepScreen();
  }
  const char* popupMessage = tr(STR_ENTERING_SLEEP);

  // Show popup with reader orientation only when going to sleep from reader
  if (APP_STATE.lastSleepFromReader) {
    ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
    showEnteringSleepPopup(renderer, popupMessage, renderTransparent);
    renderer.setOrientation(GfxRenderer::Orientation::Portrait);
  } else {
    showEnteringSleepPopup(renderer, popupMessage, renderTransparent);
  }

  if (renderTransparent) return renderTransparentSleepScreen();

  switch (SETTINGS.sleepScreen) {
    case (CrossPointSettings::SLEEP_SCREEN_MODE::BLANK):
      return renderBlankSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER):
      return renderCoverSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_STATS):
      return renderCoverSleepScreen(true);
    case (CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM_STATS):
      return renderCustomSleepScreen(true);
    case (CrossPointSettings::SLEEP_SCREEN_MODE::COVER_CUSTOM):
      if (APP_STATE.lastSleepFromReader) {
        return renderCoverSleepScreen();
      } else {
        return renderCustomSleepScreen();
      }
    case (CrossPointSettings::SLEEP_SCREEN_MODE::READING_CALENDAR):
      return renderReadingCalendarSleepScreen();
    case (CrossPointSettings::SLEEP_SCREEN_MODE::LUNAR_CALENDAR):
      return renderLunarCalendarSleepScreen();
    default:
      return renderDefaultSleepScreen();
  }
}

void SleepActivity::renderReadingCalendarSleepScreen() {
  GlobalReadingStats::LoadStatus globalStatus = GlobalReadingStats::LoadStatus::Missing;
  const GlobalReadingStats stats = GlobalReadingStats::load(&globalStatus);
  ReadingStatsDateTime now{};
  const bool clockValid = getCurrentLocalReadingStatsDateTime(now);
  if (!GlobalReadingStats::isTrustedLoadStatus(globalStatus) || !clockValid) {
    LOG_ERR("SLP", "Reading calendar unavailable: stats=%u clock=%d", static_cast<unsigned>(globalStatus), clockValid);
    return renderDefaultSleepScreen();
  }

  ReadingCalendarModel model(buildReadingCalendarSnapshot(stats, true, &now.date));
  DailyReadingHistory dailyHistory;
  const DailyReadingHistory::LoadStatus dailyStatus = DailyReadingHistory::load(dailyHistory);
  const bool dailyHistoryUsable = dailyStatus == DailyReadingHistory::LoadStatus::Ok ||
                                  dailyStatus == DailyReadingHistory::LoadStatus::Missing ||
                                  dailyStatus == DailyReadingHistory::LoadStatus::RecoveredBackup ||
                                  dailyStatus == DailyReadingHistory::LoadStatus::RecoveredTemp;
  if (!model.isAvailable() || !dailyHistoryUsable) {
    LOG_ERR("SLP", "Reading calendar unavailable: daily=%u", static_cast<unsigned>(dailyStatus));
    return renderDefaultSleepScreen();
  }
  if (model.snapshot().hasLatestDayReadingSeconds) {
    dailyHistory.reconcileExactDay(model.snapshot().latestReadingDay, model.snapshot().latestDayReadingSeconds);
  }
  model.setDailyHistory(&dailyHistory);

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};
  const int side = std::max(18, metrics.contentSidePadding);
  const Rect content{screen.x + side, screen.y + 20, std::max(1, screen.width - side * 2),
                     std::max(1, screen.height - 40)};

  char month[16];
  snprintf(month, sizeof(month), "%02u/%04u", static_cast<unsigned>(model.visibleMonth().month),
           static_cast<unsigned>(model.visibleMonth().year));
  const int monthLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int monthWidth = renderer.getTextWidth(UI_10_FONT_ID, month, EpdFontFamily::BOLD) + 18;
  const int headerHeight = std::max(renderer.getLineHeight(UI_12_FONT_ID), monthLineHeight + 10);
  const int monthX = content.x + content.width - monthWidth;
  const std::string title = renderer.truncatedText(UI_12_FONT_ID, tr(STR_READING_STATS),
                                                   std::max(1, content.width - monthWidth - 12), EpdFontFamily::BOLD);
  renderer.drawText(UI_12_FONT_ID, content.x,
                    content.y + std::max(0, (headerHeight - renderer.getLineHeight(UI_12_FONT_ID)) / 2), title.c_str(),
                    true, EpdFontFamily::BOLD);
  renderer.drawRoundedRect(monthX, content.y, monthWidth, headerHeight, 1, 5, true, true, true, true, true);
  drawCenteredInRect(renderer, UI_10_FONT_ID, Rect{monthX, content.y, monthWidth, headerHeight},
                     content.y + std::max(0, (headerHeight - monthLineHeight) / 2), month, EpdFontFamily::BOLD);

  const int titleBottom = content.y + headerHeight + 8;
  renderer.drawLine(content.x, titleBottom, content.x + content.width - 1, titleBottom);

  const ReadingCalendarMonthSummary summary = model.monthSummary();
  constexpr int sectionGap = 10;
  const int metricTop = titleBottom + sectionGap;
  const int weekdayHeight = renderer.getLineHeight(SMALL_FONT_ID) + 4;
  const ReadingCalendarGridLayout naturalGrid =
      ReadingCalendarGridLayout::calculate(Rect{0, 0, content.width, content.height}, weekdayHeight);
  const int calendarHeight = weekdayHeight + naturalGrid.grid.height;
  const int legendHeight = renderer.getLineHeight(SMALL_FONT_ID);
  const int availableMetricHeight =
      std::max(1, content.y + content.height - metricTop - calendarHeight - legendHeight - sectionGap * 3);
  int primaryMetricHeight = std::clamp(availableMetricHeight * 3 / 5, 104, 152);
  int secondaryMetricHeight = availableMetricHeight - primaryMetricHeight;
  if (secondaryMetricHeight < 58) {
    secondaryMetricHeight = std::min(58, availableMetricHeight);
    primaryMetricHeight = std::max(1, availableMetricHeight - secondaryMetricHeight);
  }

  const Rect primaryMetrics{content.x, metricTop, content.width, primaryMetricHeight};
  renderer.drawRoundedRect(primaryMetrics.x, primaryMetrics.y, primaryMetrics.width, primaryMetrics.height, 1, 6, true,
                           true, true, true, true);
  const int totalWidth = primaryMetrics.width * 54 / 100;
  const int rightHeight = primaryMetrics.height / 2;
  renderer.drawLine(primaryMetrics.x + totalWidth, primaryMetrics.y + 5, primaryMetrics.x + totalWidth,
                    primaryMetrics.y + primaryMetrics.height - 6);
  renderer.drawLine(primaryMetrics.x + totalWidth + 5, primaryMetrics.y + rightHeight,
                    primaryMetrics.x + primaryMetrics.width - 6, primaryMetrics.y + rightHeight);

  char days[12];
  snprintf(days, sizeof(days), "%u", static_cast<unsigned>(summary.readingDays));
  char streak[12];
  snprintf(streak, sizeof(streak), "%u", static_cast<unsigned>(model.snapshot().currentStreak));
  drawCalendarMetric(renderer, Rect{primaryMetrics.x, primaryMetrics.y, totalWidth, primaryMetrics.height},
                     ReadingCalendarRenderer::formatDuration(summary.totalSeconds, summary.totalIsMinimum),
                     StrId::STR_STATS_MONTH_TOTAL, UI_12_FONT_ID);
  drawCalendarMetric(
      renderer, Rect{primaryMetrics.x + totalWidth, primaryMetrics.y, primaryMetrics.width - totalWidth, rightHeight},
      days, StrId::STR_STATS_MONTH_DAYS);
  drawCalendarMetric(renderer,
                     Rect{primaryMetrics.x + totalWidth, primaryMetrics.y + rightHeight,
                          primaryMetrics.width - totalWidth, primaryMetrics.height - rightHeight},
                     streak, StrId::STR_STATS_STREAK);

  const int calendarTop = primaryMetrics.y + primaryMetrics.height + sectionGap;
  const ReadingCalendarGridLayout calendarLayout =
      ReadingCalendarGridLayout::calculate(Rect{content.x, calendarTop, content.width, calendarHeight}, weekdayHeight);
  drawCalendarWeekdays(renderer, calendarLayout);
  const Rect grid = ReadingCalendarRenderer::drawGrid(renderer, calendarLayout.grid, model, false);

  const Rect secondaryMetrics{content.x, grid.y + grid.height + sectionGap, content.width, secondaryMetricHeight};
  renderer.drawRoundedRect(secondaryMetrics.x, secondaryMetrics.y, secondaryMetrics.width, secondaryMetrics.height, 1,
                           6, true, true, true, true, true);
  const int secondaryWidth = secondaryMetrics.width / 2;
  renderer.drawLine(secondaryMetrics.x + secondaryWidth, secondaryMetrics.y + 5, secondaryMetrics.x + secondaryWidth,
                    secondaryMetrics.y + secondaryMetrics.height - 6);
  char best[32] = "--";
  if (summary.bestDayKnown) {
    const std::string duration = ReadingCalendarRenderer::formatDuration(summary.bestDaySeconds);
    snprintf(best, sizeof(best), "%s · %u", duration.c_str(), static_cast<unsigned>(summary.bestDayOfMonth));
  }
  char longest[12];
  snprintf(longest, sizeof(longest), "%u", static_cast<unsigned>(summary.longestReadingStreak));
  drawCalendarMetric(renderer, Rect{secondaryMetrics.x, secondaryMetrics.y, secondaryWidth, secondaryMetrics.height},
                     best, StrId::STR_STATS_BEST_DAY);
  drawCalendarMetric(renderer,
                     Rect{secondaryMetrics.x + secondaryWidth, secondaryMetrics.y,
                          secondaryMetrics.width - secondaryWidth, secondaryMetrics.height},
                     longest, StrId::STR_STATS_LONGEST_STREAK);

  const int legendY = secondaryMetrics.y + secondaryMetrics.height + sectionGap;
  ReadingCalendarRenderer::drawLegend(renderer, Rect{grid.x, legendY, grid.width, legendHeight});

  displayStrongSleepFrame();
}

namespace {
constexpr int LUNAR_SECTION_GAP = 10;

void formatSolarMonth(const ReadingStatsDateTime& now, char* out, const size_t size) {
  snprintf(out, size, "Tháng %u năm %u", static_cast<unsigned>(now.date.month), static_cast<unsigned>(now.date.year));
}

void formatLunarDate(const lunar::DayInfo& day, char* out, const size_t size) {
  snprintf(out, size, "%u tháng %u%s năm %s", static_cast<unsigned>(day.lunarDay),
           static_cast<unsigned>(day.lunarMonth), day.lunarLeap ? " nhuận" : "", day.yearCanChi.name);
}

void formatDayMonthCanChi(const lunar::DayInfo& day, char* out, const size_t size) {
  snprintf(out, size, "Ngày %s - Tháng %s", day.dayCanChi.name, day.monthCanChi.name);
}

// Full: day number beside the month and lunar date, then one day-quality line.
// Returns the y below the block.
int drawFullLunarDateBlock(const GfxRenderer& renderer, const Rect& content, int y, const ReadingStatsDateTime& now,
                           const lunar::DayInfo& day) {
  const int right = content.x + content.width - 1;
  const int ui10Line = renderer.getLineHeight(UI_10_FONT_ID);
  const int ui12Line = renderer.getLineHeight(UI_12_FONT_ID);

  char dayNumber[4];
  snprintf(dayNumber, sizeof(dayNumber), "%u", static_cast<unsigned>(now.date.day));
  const int dayWidth = renderer.getTextWidth(SLEEP_DIGITS_48_FONT_ID, dayNumber);
  const int dayHeight = digitInkHeight(renderer, SLEEP_DIGITS_48_FONT_ID);
  char solarMonth[32];
  formatSolarMonth(now, solarMonth, sizeof(solarMonth));
  char lunarDate[64];
  formatLunarDate(day, lunarDate, sizeof(lunarDate));
  char canChi[56];
  formatDayMonthCanChi(day, canChi, sizeof(canChi));
  const int textHeight = ui12Line * 2 + ui10Line + 8;
  const int blockHeight = std::max(dayHeight, textHeight);
  drawDigits(renderer, SLEEP_DIGITS_48_FONT_ID, content.x + 4, y + (blockHeight - dayHeight) / 2, dayNumber);
  const int textX = content.x + 4 + dayWidth + 20;
  const int textWidth = std::max(1, right + 1 - textX);
  int textY = y + (blockHeight - textHeight) / 2;
  drawFittedLine(renderer, textX, textY, textWidth, {solarMonth}, {UI_12_FONT_ID, UI_10_FONT_ID});
  textY += ui12Line + 4;
  drawFittedLine(renderer, textX, textY, textWidth, {lunarDate}, {UI_12_FONT_ID, UI_10_FONT_ID, SMALL_FONT_ID},
                 EpdFontFamily::BOLD);
  textY += ui12Line + 4;
  drawFittedLine(renderer, textX, textY, textWidth, {canChi}, {UI_10_FONT_ID, SMALL_FONT_ID});
  y += blockHeight + LUNAR_SECTION_GAP;
  renderer.drawLine(content.x, y, right, y);
  y += LUNAR_SECTION_GAP;

  // Day quality: hoàng/hắc đạo, trực, star and the current or next good hour.
  const lunar::HourSlot hour = lunar::currentOrNextHoangDaoHour(day, now.hour);
  const char* hourLabel = hour.current ? "Giờ tốt:" : "Giờ tốt tới:";
  char hourText[40];
  snprintf(hourText, sizeof(hourText), "%s %s %u-%uh", hourLabel, lunar::chiName(hour.chi),
           static_cast<unsigned>(hour.startHour), static_cast<unsigned>((hour.startHour + 2) % 24));
  const char* dayKind = day.hoangDao ? "Hoàng đạo" : "Hắc đạo";
  char qualityFull[128];
  snprintf(qualityFull, sizeof(qualityFull), "%s (%s) - Trực %s - Sao %s - %s", dayKind, lunar::deityName(day.deity),
           lunar::trucName(day.truc), lunar::starName(day.star), hourText);
  char qualityShort[112];
  snprintf(qualityShort, sizeof(qualityShort), "%s - Trực %s - Sao %s - %s", dayKind, lunar::trucName(day.truc),
           lunar::starName(day.star), hourText);
  char qualityMinimal[96];
  snprintf(qualityMinimal, sizeof(qualityMinimal), "%s - Trực %s - Sao %s - %s %s", dayKind, lunar::trucName(day.truc),
           lunar::starName(day.star), hourLabel, lunar::chiName(hour.chi));
  drawFittedLine(renderer, content.x, y, content.width, {qualityFull, qualityShort, qualityMinimal},
                 {UI_10_FONT_ID, SMALL_FONT_ID});
  return y + ui10Line + LUNAR_SECTION_GAP;
}

// Minimal: the solar day alone in large type, its lunar counterpart below.
void drawMinimalLunarLayout(const GfxRenderer& renderer, const Rect& area, const ReadingStatsDateTime& now,
                            const lunar::DayInfo& day) {
  const int ui10Line = renderer.getLineHeight(UI_10_FONT_ID);
  const int ui12Line = renderer.getLineHeight(UI_12_FONT_ID);
  const int dayHeight = digitInkHeight(renderer, SLEEP_DIGITS_88_FONT_ID);
  constexpr int ruleWidth = 48;
  const int blockHeight = ui12Line + 24 + dayHeight + 28 + ui12Line + 36 + 1 + 36 + ui12Line + 6 + ui10Line;
  // Sit a little above the geometric center, as printed calendars do.
  int y = area.y + std::max(0, (area.height - blockHeight) * 2 / 5);

  drawCenteredFittedLine(renderer, area, y, lunar::weekdayName(day.weekday), {UI_12_FONT_ID}, EpdFontFamily::BOLD);
  y += ui12Line + 24;
  char dayNumber[4];
  snprintf(dayNumber, sizeof(dayNumber), "%u", static_cast<unsigned>(now.date.day));
  drawDigits(renderer, SLEEP_DIGITS_88_FONT_ID,
             area.x + (area.width - renderer.getTextWidth(SLEEP_DIGITS_88_FONT_ID, dayNumber)) / 2, y, dayNumber);
  y += dayHeight + 28;
  char solarMonth[32];
  formatSolarMonth(now, solarMonth, sizeof(solarMonth));
  drawCenteredFittedLine(renderer, area, y, solarMonth, {UI_12_FONT_ID, UI_10_FONT_ID});
  y += ui12Line + 36;
  renderer.drawLine(area.x + (area.width - ruleWidth) / 2, y, area.x + (area.width + ruleWidth) / 2, y);
  y += 1 + 36;
  char lunarDate[64];
  formatLunarDate(day, lunarDate, sizeof(lunarDate));
  drawCenteredFittedLine(renderer, area, y, lunarDate, {UI_12_FONT_ID, UI_10_FONT_ID, SMALL_FONT_ID},
                         EpdFontFamily::BOLD);
  y += ui12Line + 6;
  char canChi[56];
  formatDayMonthCanChi(day, canChi, sizeof(canChi));
  drawCenteredFittedLine(renderer, area, y, canChi, {UI_10_FONT_ID, SMALL_FONT_ID});
}

// Tear-off: a Vietnamese wall-calendar page carrying the day's almanac and
// every hoàng đạo hour.
void drawTearOffLunarLayout(const GfxRenderer& renderer, const Rect& area, const ReadingStatsDateTime& now,
                            const lunar::DayInfo& day) {
  const int ui10Line = renderer.getLineHeight(UI_10_FONT_ID);
  const int ui12Line = renderer.getLineHeight(UI_12_FONT_ID);
  const int dayHeight = digitInkHeight(renderer, SLEEP_DIGITS_88_FONT_ID);
  const int lunarDayHeight = digitInkHeight(renderer, SLEEP_DIGITS_48_FONT_ID);
  const int sideTextHeight = ui12Line + 4 + ui10Line;
  const int columnHeight = std::max(lunarDayHeight, sideTextHeight);
  constexpr int HOUR_COLUMNS = 3;
  std::array<uint8_t, 12> hours{};
  int hourCount = 0;
  for (uint8_t chi = 0; chi < hours.size(); ++chi) {
    if (day.hoangDaoHours & (1u << chi)) hours[hourCount++] = chi;
  }
  const int hourRows = (hourCount + HOUR_COLUMNS - 1) / HOUR_COLUMNS;
  const int hourRowHeight = ui10Line + 4;

  // Spread the spare height over the gaps between sections (7.5 gap units),
  // then center the whole page in what is left.
  const int fixedHeight = ui12Line + 8 + dayHeight + ui12Line + 1 + columnHeight + ui12Line + 1 + ui12Line + 4 +
                          ui10Line + ui10Line + 4 + hourRows * hourRowHeight;
  const int gap = std::clamp((area.height - fixedHeight) * 2 / 15, 8, 32);
  int y = area.y + std::max(0, (area.height - fixedHeight - gap * 15 / 2) / 2);
  const int right = area.x + area.width - 1;

  char solarMonth[32];
  formatSolarMonth(now, solarMonth, sizeof(solarMonth));
  drawCenteredFittedLine(renderer, area, y, solarMonth, {UI_12_FONT_ID, UI_10_FONT_ID}, EpdFontFamily::BOLD);
  y += ui12Line + 6;
  renderer.fillRect(area.x, y, area.width, 2);
  y += 2 + gap;

  char dayNumber[4];
  snprintf(dayNumber, sizeof(dayNumber), "%u", static_cast<unsigned>(now.date.day));
  drawDigits(renderer, SLEEP_DIGITS_88_FONT_ID,
             area.x + (area.width - renderer.getTextWidth(SLEEP_DIGITS_88_FONT_ID, dayNumber)) / 2, y, dayNumber);
  y += dayHeight + gap;
  drawCenteredFittedLine(renderer, area, y, lunar::weekdayName(day.weekday), {UI_12_FONT_ID}, EpdFontFamily::BOLD);
  y += ui12Line + gap;
  renderer.drawLine(area.x, y, right, y);
  y += 1 + gap;

  // Lunar row: month with its can chi, the lunar day, then the year.
  const int columnWidth = area.width / 3;
  const Rect monthColumn{area.x, y, columnWidth, columnHeight};
  const Rect dayColumn{area.x + columnWidth, y, area.width - columnWidth * 2, columnHeight};
  const Rect yearColumn{dayColumn.x + dayColumn.width, y, columnWidth, columnHeight};
  const int sideTextY = y + (columnHeight - sideTextHeight) / 2;
  char lunarMonth[24];
  snprintf(lunarMonth, sizeof(lunarMonth), "Tháng %u%s", static_cast<unsigned>(day.lunarMonth),
           day.lunarLeap ? " nhuận" : "");
  drawCenteredFittedLine(renderer, monthColumn, sideTextY, lunarMonth, {UI_12_FONT_ID, UI_10_FONT_ID},
                         EpdFontFamily::BOLD);
  drawCenteredFittedLine(renderer, monthColumn, sideTextY + ui12Line + 4, day.monthCanChi.name,
                         {UI_10_FONT_ID, SMALL_FONT_ID});
  char lunarDay[4];
  snprintf(lunarDay, sizeof(lunarDay), "%u", static_cast<unsigned>(day.lunarDay));
  drawDigits(renderer, SLEEP_DIGITS_48_FONT_ID,
             dayColumn.x + (dayColumn.width - renderer.getTextWidth(SLEEP_DIGITS_48_FONT_ID, lunarDay)) / 2,
             y + (columnHeight - lunarDayHeight) / 2, lunarDay);
  drawCenteredFittedLine(renderer, yearColumn, sideTextY, "Năm", {UI_12_FONT_ID}, EpdFontFamily::BOLD);
  drawCenteredFittedLine(renderer, yearColumn, sideTextY + ui12Line + 4, day.yearCanChi.name,
                         {UI_10_FONT_ID, SMALL_FONT_ID});
  renderer.drawLine(dayColumn.x, y + 4, dayColumn.x, y + columnHeight - 4);
  renderer.drawLine(yearColumn.x, y + 4, yearColumn.x, y + columnHeight - 4);
  y += columnHeight + gap / 2;
  char dayCanChi[32];
  snprintf(dayCanChi, sizeof(dayCanChi), "Ngày %s", day.dayCanChi.name);
  drawCenteredFittedLine(renderer, area, y, dayCanChi, {UI_12_FONT_ID}, EpdFontFamily::BOLD);
  y += ui12Line + gap;
  renderer.drawLine(area.x, y, right, y);
  y += 1 + gap;

  char dayKind[48];
  snprintf(dayKind, sizeof(dayKind), "%s (%s)", day.hoangDao ? "Hoàng đạo" : "Hắc đạo", lunar::deityName(day.deity));
  drawCenteredFittedLine(renderer, area, y, dayKind, {UI_12_FONT_ID, UI_10_FONT_ID}, EpdFontFamily::BOLD);
  y += ui12Line + 4;
  char trucStar[48];
  snprintf(trucStar, sizeof(trucStar), "Trực %s - Sao %s", lunar::trucName(day.truc), lunar::starName(day.star));
  drawCenteredFittedLine(renderer, area, y, trucStar, {UI_10_FONT_ID, SMALL_FONT_ID});
  y += ui10Line + gap;

  drawCenteredFittedLine(renderer, area, y, "Giờ hoàng đạo", {UI_10_FONT_ID}, EpdFontFamily::BOLD);
  y += ui10Line + 4;
  for (int index = 0; index < hourCount; ++index) {
    const uint8_t chi = hours[index];
    const unsigned start = (23u + 2u * chi) % 24u;
    char label[24];
    snprintf(label, sizeof(label), "%s %u-%uh", lunar::chiName(chi), start, (start + 2u) % 24u);
    const int column = index % HOUR_COLUMNS;
    const int cellX = area.x + area.width * column / HOUR_COLUMNS;
    const Rect cell{cellX, y + (index / HOUR_COLUMNS) * hourRowHeight,
                    area.x + area.width * (column + 1) / HOUR_COLUMNS - cellX, hourRowHeight};
    drawCenteredFittedLine(renderer, cell, cell.y, label, {UI_10_FONT_ID, SMALL_FONT_ID});
  }
}

constexpr char BRIEF_PATH[] = "/.crosspoint/brief.json";
constexpr char QUOTES_PATH[] = "/quotes.txt";
constexpr size_t MAX_QUOTES_FILE_BYTES = 32 * 1024;
constexpr char ELLIPSIS[] = "\xE2\x80\xA6";

struct BriefFooterInfo {
  bool streakKnown = false;
  uint16_t streak = 0;
  bool tetKnown = false;
  sleep_brief::TetStatus tet;
};

// A missing or rejected file is the normal "no network yet" state, not an error.
bool loadBrief(sleep_brief::Brief& brief) {
  if (!Storage.exists(BRIEF_PATH)) return false;
  auto buffer = makeUniqueNoThrow<char[]>(sleep_brief::MAX_BRIEF_BYTES + 2);
  if (!buffer) return false;
  // One byte past the cap is read on purpose so an oversize file is detected.
  const size_t length = Storage.readFileToBuffer(BRIEF_PATH, buffer.get(), sleep_brief::MAX_BRIEF_BYTES + 2);
  const sleep_brief::ParseStatus status = sleep_brief::parseBrief(buffer.get(), length, brief);
  if (status != sleep_brief::ParseStatus::Ok) {
    LOG_ERR("SLP", "brief.json rejected: status=%u", static_cast<unsigned>(status));
    brief = sleep_brief::Brief{};
    return false;
  }
  return true;
}

sleep_brief::Quote loadQuoteOfTheDay(const sleep_brief::Brief* brief, const uint32_t dayOfYear) {
  if (brief && brief->hasQuote) return brief->quote;
  sleep_brief::Quote quote;
  if (Storage.exists(QUOTES_PATH)) {
    auto buffer = makeUniqueNoThrow<char[]>(MAX_QUOTES_FILE_BYTES + 1);
    if (buffer) {
      const size_t length = Storage.readFileToBuffer(QUOTES_PATH, buffer.get(), MAX_QUOTES_FILE_BYTES + 1);
      if (sleep_brief::pickQuoteFromText(buffer.get(), length, dayOfYear, quote)) return quote;
    }
  }
  return sleep_brief::builtinQuote(dayOfYear);
}

// Read-only: only current-format stores in the Loaded state are touched, so no
// recovery or migration write can happen while the device is going to sleep.
bool loadHighlightOfTheDay(const uint32_t dayOfYear, sleep_brief::Quote& out) {
  auto catalog = makeUniqueNoThrow<ClippingStore::Catalog>();
  if (!catalog || ClippingStore::loadCatalog(*catalog) != ClippingStore::CatalogLoadResult::Loaded) return false;
  uint32_t total = 0;
  for (const auto& entry : catalog->entries) {
    if (entry.format == ClippingCodec::Format::Current) total += entry.clippingCount;
  }
  if (total == 0) return false;
  uint32_t ordinal = (dayOfYear * 2654435761u) % total;
  for (const auto& entry : catalog->entries) {
    if (entry.format != ClippingCodec::Format::Current) continue;
    if (ordinal >= entry.clippingCount) {
      ordinal -= entry.clippingCount;
      continue;
    }
    auto store = makeUniqueNoThrow<ClippingStore>();
    if (!store || store->loadForBook(entry.book.path, entry.book.title, entry.book.author, entry.book.bookType) !=
                      ClippingStore::LoadResult::Loaded) {
      return false;
    }
    std::string text;
    if (ordinal >= store->size() || !store->readText(ordinal, text) || text.empty()) return false;
    out.text = std::move(text);
    out.by = entry.book.title;
    return true;
  }
  return false;
}

BriefFooterInfo loadBriefFooterInfo(const ReadingStatsDateTime& now) {
  BriefFooterInfo info;
  GlobalReadingStats::LoadStatus status = GlobalReadingStats::LoadStatus::Missing;
  const GlobalReadingStats stats = GlobalReadingStats::load(&status);
  if (GlobalReadingStats::isTrustedLoadStatus(status)) {
    info.streakKnown = true;
    info.streak = stats.currentReadingStreak(&now.date);
  }
  info.tetKnown = sleep_brief::tetStatus(now.date.year, now.date.month, now.date.day, info.tet);
  return info;
}

bool endsWithEllipsis(const std::string& text) {
  return text.size() >= 3 && text.compare(text.size() - 3, 3, ELLIPSIS) == 0;
}

// Wraps the quote in the largest font whose wrapped text fits the area (the
// smallest font truncates), then centers it with the author line below.
void drawQuoteBlock(const GfxRenderer& renderer, const Rect& area, const sleep_brief::Quote& quote,
                    std::initializer_list<int> fonts) {
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int byHeight = quote.by.empty() ? 0 : smallLine + 8;
  const int textWidth = area.width - 16;
  if (textWidth < 40) return;
  std::vector<std::string> lines;
  int fontId = *(fonts.end() - 1);
  for (const int candidate : fonts) {
    const int maxLines = (area.height - byHeight) / renderer.getLineHeight(candidate);
    if (maxLines < 1) continue;
    lines = renderer.wrappedText(candidate, quote.text.c_str(), textWidth, maxLines);
    fontId = candidate;
    if (lines.empty() || !endsWithEllipsis(lines.back()) || endsWithEllipsis(quote.text)) break;
  }
  if (lines.empty()) return;
  const int lineHeight = renderer.getLineHeight(fontId);
  const int blockHeight = static_cast<int>(lines.size()) * lineHeight + byHeight;
  int y = area.y + std::max(0, (area.height - blockHeight) / 2);
  for (const std::string& line : lines) {
    drawCenteredInRect(renderer, fontId, area, y, line.c_str());
    y += lineHeight;
  }
  if (!quote.by.empty()) {
    const std::string by = "— " + quote.by;
    drawCenteredFittedLine(renderer, Rect{area.x + 8, area.y, textWidth, area.height}, y + 8, by.c_str(),
                           {SMALL_FONT_ID});
  }
}

int statCellHeight(const GfxRenderer& renderer, const int valueFontId) {
  return renderer.getLineHeight(valueFontId) + renderer.getLineHeight(SMALL_FONT_ID) + 10;
}

void drawStatCell(const GfxRenderer& renderer, const Rect& rect, const char* value, const char* label,
                  const int valueFontId) {
  const Rect inner{rect.x + 4, rect.y, std::max(1, rect.width - 8), rect.height};
  const int contentHeight = renderer.getLineHeight(valueFontId) + 2 + renderer.getLineHeight(SMALL_FONT_ID);
  const int top = rect.y + std::max(0, (rect.height - contentHeight) / 2);
  drawCenteredFittedLine(renderer, inner, top, value[0] ? value : "--", {valueFontId, UI_10_FONT_ID},
                         EpdFontFamily::BOLD);
  drawCenteredFittedLine(renderer, inner, top + renderer.getLineHeight(valueFontId) + 2, label, {SMALL_FONT_ID});
}

// "label value" centered in `rect`, the value bold and baseline-aligned.
void drawLabelValue(const GfxRenderer& renderer, const Rect& rect, const int y, const char* label, const char* value,
                    const int valueFontId, const int labelFontId = UI_10_FONT_ID) {
  const char* shownValue = value[0] ? value : "--";
  const int labelWidth = renderer.getTextWidth(labelFontId, label);
  const int valueWidth = renderer.getTextWidth(valueFontId, shownValue, EpdFontFamily::BOLD);
  const int total = labelWidth + 8 + valueWidth;
  if (total > rect.width - 8) {
    drawCenteredFittedLine(renderer, rect, y, shownValue, {valueFontId, UI_10_FONT_ID}, EpdFontFamily::BOLD);
    return;
  }
  const int x = rect.x + (rect.width - total) / 2;
  const int labelY = y + renderer.getFontAscenderSize(valueFontId) - renderer.getFontAscenderSize(labelFontId);
  renderer.drawText(labelFontId, x, labelY, label);
  renderer.drawText(valueFontId, x + labelWidth + 8, y, shownValue, true, EpdFontFamily::BOLD);
}

int drawSectionHeading(const GfxRenderer& renderer, const Rect& content, const int y, const char* title) {
  const int line = renderer.getLineHeight(UI_10_FONT_ID);
  renderer.drawText(UI_10_FONT_ID, content.x, y, title, true, EpdFontFamily::BOLD);
  renderer.drawLine(content.x, y + line, content.x + content.width - 1, y + line);
  return y + line + 7;
}

void drawTruncatedRow(const GfxRenderer& renderer, const int x, const int y, const int width, const char* text,
                      const EpdFontFamily::Style style = EpdFontFamily::REGULAR) {
  const std::string shown = renderer.truncatedText(UI_10_FONT_ID, text, std::max(1, width), style);
  renderer.drawText(UI_10_FONT_ID, x, y, shown.c_str(), true, style);
}

// Shared by the Brief and Daily review presets. `blocks` is null (or empty of
// network data) when only the quote and the footer should fill the page.
void drawBriefFamilyLayout(const GfxRenderer& renderer, const Rect& content, const int statusHeight,
                           const ReadingStatsDateTime& now, const lunar::DayInfo& day, const sleep_brief::Brief* blocks,
                           const sleep_brief::Quote& quote, const BriefFooterInfo& footer) {
  constexpr int gap = LUNAR_SECTION_GAP;
  const int right = content.x + content.width - 1;
  const int bottom = content.y + content.height;
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int ui10Line = renderer.getLineHeight(UI_10_FONT_ID);
  const int ui12Line = renderer.getLineHeight(UI_12_FONT_ID);

  char solarDate[40];
  snprintf(solarDate, sizeof(solarDate), "%s - %02u/%02u/%04u", lunar::weekdayName(day.weekday),
           static_cast<unsigned>(now.date.day), static_cast<unsigned>(now.date.month),
           static_cast<unsigned>(now.date.year));
  renderer.drawText(SMALL_FONT_ID, right + 1 - renderer.getTextWidth(SMALL_FONT_ID, solarDate, EpdFontFamily::BOLD),
                    content.y, solarDate, true, EpdFontFamily::BOLD);
  int y = content.y + statusHeight + 6;
  renderer.drawLine(content.x, y, right, y);
  y += gap + 4;

  char lunarDate[64];
  formatLunarDate(day, lunarDate, sizeof(lunarDate));
  char lunarLine[80];
  snprintf(lunarLine, sizeof(lunarLine), "Âm lịch %s", lunarDate);
  drawCenteredFittedLine(renderer, content, y, lunarLine, {UI_12_FONT_ID, UI_10_FONT_ID, SMALL_FONT_ID},
                         EpdFontFamily::BOLD);
  y += ui12Line + 4;
  char canChi[56];
  formatDayMonthCanChi(day, canChi, sizeof(canChi));
  drawCenteredFittedLine(renderer, content, y, canChi, {UI_10_FONT_ID, SMALL_FONT_ID});
  y += ui10Line + gap;

  // Footer is anchored to the bottom; the quote takes whatever height is left.
  const bool showUpdated = blocks && !blocks->updated.empty();
  const int updatedHeight = showUpdated ? smallLine + 6 : 0;
  const int statsHeight = statCellHeight(renderer, UI_12_FONT_ID);
  const int statsTop = bottom - updatedHeight - statsHeight;
  const int ruleY = statsTop - gap;
  const int quoteBottom = ruleY - gap;
  const bool showBlocks = blocks && blocks->hasNetworkBlocks();
  // Network blocks give way (in order) before the quote loses its last two lines.
  const int blockLimit = quoteBottom - (ui10Line * 2 + gap) - gap;
  const auto fits = [&](const int height) { return y + height <= blockLimit; };
  const int headingHeight = ui10Line + 7;

  if (showBlocks) {
    const int rowHeight = ui10Line + 6;
    if (!blocks->weather.empty() && fits(headingHeight + static_cast<int>(blocks->weather.size()) * rowHeight)) {
      y = drawSectionHeading(renderer, content, y, "Thời tiết");
      for (const auto& w : blocks->weather) {
        const int rangeWidth = renderer.getTextWidth(UI_10_FONT_ID, w.range.c_str(), EpdFontFamily::BOLD);
        const int cityMax = content.width - rangeWidth - 12;
        const std::string city = renderer.truncatedText(UI_10_FONT_ID, w.city.c_str(), cityMax, EpdFontFamily::BOLD);
        renderer.drawText(UI_10_FONT_ID, content.x, y, city.c_str(), true, EpdFontFamily::BOLD);
        renderer.drawText(UI_10_FONT_ID, right + 1 - rangeWidth, y, w.range.c_str(), true, EpdFontFamily::BOLD);
        const int nowX = content.x + renderer.getTextWidth(UI_10_FONT_ID, city.c_str(), EpdFontFamily::BOLD) + 12;
        const int nowWidth = right + 1 - rangeWidth - 12 - nowX;
        if (nowWidth > 24) drawTruncatedRow(renderer, nowX, y, nowWidth, w.now.c_str());
        y += rowHeight;
      }
      y += gap - 6;
    }
    if (blocks->hasGold && fits(headingHeight + ui12Line + ui10Line + 16)) {
      const auto& g = blocks->gold;
      const std::string title = g.unit.empty() ? g.name : g.name + " (" + g.unit + ")";
      y = drawSectionHeading(renderer, content, y, title.c_str());
      const Rect buy{content.x, y, content.width / 2, ui12Line};
      const Rect sell{content.x + content.width / 2, y, content.width - content.width / 2, ui12Line};
      drawLabelValue(renderer, buy, y, "Mua", g.buy.c_str(), UI_12_FONT_ID);
      drawLabelValue(renderer, sell, y, "Bán", g.sell.c_str(), UI_12_FONT_ID);
      y += ui12Line + 8;
      const char* labels[3] = {"1 tháng", "3 tháng", "1 năm"};
      const std::string* values[3] = {&g.d1m, &g.d3m, &g.d1y};
      for (int index = 0; index < 3; ++index) {
        const int x = content.x + content.width * index / 3;
        const Rect cell{x, y, content.x + content.width * (index + 1) / 3 - x, ui10Line};
        drawLabelValue(renderer, cell, y, labels[index], values[index]->c_str(), UI_10_FONT_ID, SMALL_FONT_ID);
      }
      y += ui10Line + 8 + gap - 6;
    }
    if (!blocks->news.empty() && fits(headingHeight + static_cast<int>(blocks->news.size()) * rowHeight)) {
      y = drawSectionHeading(renderer, content, y, "Tin AI");
      for (const auto& item : blocks->news) {
        const std::string line = "- " + item;
        drawTruncatedRow(renderer, content.x, y, content.width, line.c_str());
        y += rowHeight;
      }
      y += gap - 6;
    }
    if (!blocks->events.empty() && fits(headingHeight + static_cast<int>(blocks->events.size()) * rowHeight)) {
      y = drawSectionHeading(renderer, content, y, "Hôm nay");
      for (const auto& event : blocks->events) {
        int textX = content.x;
        if (!event.time.empty()) {
          renderer.drawText(UI_10_FONT_ID, content.x, y, event.time.c_str(), true, EpdFontFamily::BOLD);
          textX += renderer.getTextWidth(UI_10_FONT_ID, "00:00", EpdFontFamily::BOLD) + 12;
        }
        drawTruncatedRow(renderer, textX, y, right + 1 - textX, event.text.c_str());
        y += rowHeight;
      }
      y += gap - 6;
    }
  }

  const Rect quoteArea{content.x, y, content.width, std::max(0, quoteBottom - y)};
  if (showBlocks) {
    drawQuoteBlock(renderer, quoteArea, quote, {UI_12_FONT_ID, UI_10_FONT_ID});
  } else {
    drawQuoteBlock(renderer, quoteArea, quote,
                   {NOTOSERIF_18_FONT_ID, NOTOSERIF_16_FONT_ID, NOTOSERIF_14_FONT_ID, UI_12_FONT_ID, UI_10_FONT_ID});
  }

  renderer.drawLine(content.x, ruleY, right, ruleY);
  const int cellCount = (footer.streakKnown ? 1 : 0) + (footer.tetKnown ? 1 : 0);
  int cellIndex = 0;
  const auto nextCell = [&]() {
    const int x = content.x + content.width * cellIndex / cellCount;
    const Rect cell{x, statsTop, content.x + content.width * (cellIndex + 1) / cellCount - x, statsHeight};
    ++cellIndex;
    return cell;
  };
  if (footer.streakKnown) {
    char value[24];
    snprintf(value, sizeof(value), "%u ngày", static_cast<unsigned>(footer.streak));
    drawStatCell(renderer, nextCell(), value, "Chuỗi đọc", UI_12_FONT_ID);
  }
  if (footer.tetKnown) {
    char value[24];
    if (footer.tet.greeting) {
      drawStatCell(renderer, nextCell(), "Chúc mừng năm mới", "Tết Nguyên Đán", UI_12_FONT_ID);
    } else {
      snprintf(value, sizeof(value), "%d ngày", footer.tet.daysUntil);
      drawStatCell(renderer, nextCell(), value, "Đến Tết Nguyên Đán", UI_12_FONT_ID);
    }
  }
  if (showUpdated) {
    char updated[40];
    snprintf(updated, sizeof(updated), "Cập nhật %s", blocks->updated.c_str());
    drawCenteredFittedLine(renderer, content, bottom - smallLine, updated, {SMALL_FONT_ID});
  }
}
}  // namespace

// Lunar terms are Vietnamese by nature and the lunar library names are
// Vietnamese only, so this screen's calendar text is not routed through I18N.
void SleepActivity::renderLunarCalendarSleepScreen() {
  ReadingStatsDateTime now{};
  lunar::DayInfo day{};
  if (!getCurrentLocalReadingStatsDateTime(now) ||
      !lunar::computeDay(now.date.year, now.date.month, now.date.day, day)) {
    LOG_ERR("SLP", "Lunar calendar unavailable: clock invalid");
    return renderReadingCalendarSleepScreen();
  }

  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect screen{0, 0, renderer.getScreenWidth(), renderer.getScreenHeight()};
  const int side = std::max(18, metrics.contentSidePadding);
  const Rect content{screen.x + side, screen.y + 16, std::max(1, screen.width - side * 2),
                     std::max(1, screen.height - 32)};
  const int right = content.x + content.width - 1;
  const int bottom = content.y + content.height;
  const int smallLine = renderer.getLineHeight(SMALL_FONT_ID);
  const int ui10Line = renderer.getLineHeight(UI_10_FONT_ID);
  constexpr int sectionGap = LUNAR_SECTION_GAP;
  const uint8_t layout = SETTINGS.sleepCalendarLayout < CrossPointSettings::SLEEP_CALENDAR_LAYOUT_COUNT
                             ? SETTINGS.sleepCalendarLayout
                             : CrossPointSettings::CALENDAR_LAYOUT_FULL;

  GUI.drawBatteryLeft(renderer, Rect{content.x, content.y, metrics.batteryWidth, metrics.batteryHeight},
                      powerManager.getBatteryPercentage(),
                      SETTINGS.hideBatteryPercentage != CrossPointSettings::HIDE_BATTERY_PERCENTAGE::HIDE_ALWAYS);
  if (layout == CrossPointSettings::CALENDAR_LAYOUT_BRIEF ||
      layout == CrossPointSettings::CALENDAR_LAYOUT_DAILY_REVIEW) {
    const bool isBrief = layout == CrossPointSettings::CALENDAR_LAYOUT_BRIEF;
    const uint32_t dayOfYear = sleep_brief::dayOfYear(now.date.year, now.date.month, now.date.day);
    auto brief = makeUniqueNoThrow<sleep_brief::Brief>();
    const bool haveBrief = brief && loadBrief(*brief);
    // Daily review shows a saved highlight on odd days and the quote otherwise.
    sleep_brief::Quote quote;
    const bool highlight = !isBrief && (dayOfYear % 2 == 1) && loadHighlightOfTheDay(dayOfYear, quote);
    if (!highlight) quote = loadQuoteOfTheDay(haveBrief ? brief.get() : nullptr, dayOfYear);
    const BriefFooterInfo footer = loadBriefFooterInfo(now);
    drawBriefFamilyLayout(renderer, content, std::max(smallLine, metrics.batteryHeight + 6), now, day,
                          isBrief && haveBrief ? brief.get() : nullptr, quote, footer);
    displayStrongSleepFrame();
    return;
  }
  if (layout == CrossPointSettings::CALENDAR_LAYOUT_MINIMAL || layout == CrossPointSettings::CALENDAR_LAYOUT_TEAR_OFF) {
    // These layouts carry no reading stats, so the stats files stay unread.
    const int top = content.y + metrics.batteryHeight + sectionGap;
    const Rect area{content.x, top, content.width, std::max(1, bottom - top)};
    if (layout == CrossPointSettings::CALENDAR_LAYOUT_MINIMAL) {
      drawMinimalLunarLayout(renderer, area, now, day);
    } else {
      drawTearOffLunarLayout(renderer, area, now, day);
    }
    displayStrongSleepFrame();
    return;
  }

  GlobalReadingStats::LoadStatus globalStatus = GlobalReadingStats::LoadStatus::Missing;
  GlobalReadingStats stats = GlobalReadingStats::load(&globalStatus);
  const bool statsTrusted = GlobalReadingStats::isTrustedLoadStatus(globalStatus);
  // Unreadable stats must not hide the calendar itself; show an empty heatmap.
  if (!statsTrusted) stats = GlobalReadingStats{};
  ReadingCalendarModel model(buildReadingCalendarSnapshot(stats, true, &now.date));
  DailyReadingHistory dailyHistory;
  const DailyReadingHistory::LoadStatus dailyStatus = DailyReadingHistory::load(dailyHistory);
  const bool dailyHistoryUsable = dailyStatus == DailyReadingHistory::LoadStatus::Ok ||
                                  dailyStatus == DailyReadingHistory::LoadStatus::Missing ||
                                  dailyStatus == DailyReadingHistory::LoadStatus::RecoveredBackup ||
                                  dailyStatus == DailyReadingHistory::LoadStatus::RecoveredTemp;
  if (dailyHistoryUsable) {
    if (model.snapshot().hasLatestDayReadingSeconds) {
      dailyHistory.reconcileExactDay(model.snapshot().latestReadingDay, model.snapshot().latestDayReadingSeconds);
    }
    model.setDailyHistory(&dailyHistory);
  }
  const bool showReadingStats = statsTrusted && dailyHistoryUsable && model.isAvailable();
  if (!showReadingStats) {
    LOG_ERR("SLP", "Lunar calendar without reading stats: stats=%u daily=%u", static_cast<unsigned>(globalStatus),
            static_cast<unsigned>(dailyStatus));
  }

  // Status bar: weekday and solar date beside the battery.
  char solarDate[40];
  snprintf(solarDate, sizeof(solarDate), "%s - %02u/%02u/%04u", lunar::weekdayName(day.weekday),
           static_cast<unsigned>(now.date.day), static_cast<unsigned>(now.date.month),
           static_cast<unsigned>(now.date.year));
  renderer.drawText(SMALL_FONT_ID, right + 1 - renderer.getTextWidth(SMALL_FONT_ID, solarDate, EpdFontFamily::BOLD),
                    content.y, solarDate, true, EpdFontFamily::BOLD);
  int y = content.y + std::max(smallLine, metrics.batteryHeight + 6) + 6;
  renderer.drawLine(content.x, y, right, y);
  y += sectionGap + 4;

  if (layout == CrossPointSettings::CALENDAR_LAYOUT_READING) {
    // Reading: one compact lunar line leaves the height to the month grid.
    char lunarDate[64];
    formatLunarDate(day, lunarDate, sizeof(lunarDate));
    char lunarLine[112];
    snprintf(lunarLine, sizeof(lunarLine), "Âm lịch %s - Ngày %s", lunarDate, day.dayCanChi.name);
    drawFittedLine(renderer, content.x, y, content.width, {lunarLine, lunarDate}, {UI_10_FONT_ID, SMALL_FONT_ID});
    y += ui10Line + sectionGap;
  } else {
    y = drawFullLunarDateBlock(renderer, content, y, now, day);
  }

  // Month grid; metric rows give way from the bottom before cells get too
  // small to carry the lunar day numbers.
  const int weekdayHeight = smallLine + 4;
  const int legendHeight = smallLine;
  const int metricRowHeight = ui10Line + smallLine + 16;
  const ReadingCalendarGridLayout naturalGrid =
      ReadingCalendarGridLayout::calculate(Rect{0, 0, content.width, content.height}, weekdayHeight);
  int metricRows = showReadingStats ? 2 : 0;
  ReadingCalendarGridLayout calendarLayout;
  while (true) {
    const int reserved =
        (showReadingStats ? legendHeight + sectionGap : 0) + metricRows * (metricRowHeight + sectionGap);
    const int calendarHeight =
        std::min(weekdayHeight + naturalGrid.grid.height, std::max(0, bottom - y - sectionGap - reserved));
    calendarLayout =
        ReadingCalendarGridLayout::calculate(Rect{content.x, y, content.width, calendarHeight}, weekdayHeight);
    if (metricRows == 0 || calendarLayout.cellSize >= minLunarCellSize(renderer)) break;
    --metricRows;
  }
  drawCalendarWeekdays(renderer, calendarLayout);
  const Rect grid = ReadingCalendarRenderer::drawGrid(renderer, calendarLayout.grid, model, false);
  drawLunarDayNumbers(renderer, calendarLayout, model);
  y = grid.y + grid.height + sectionGap;
  if (showReadingStats) {
    ReadingCalendarRenderer::drawLegend(renderer, Rect{grid.x, y, grid.width, legendHeight});
    y += legendHeight + sectionGap;
  }

  // Reading metrics, the same values the reading-stats sleep screen shows.
  const ReadingCalendarMonthSummary summary = model.monthSummary();
  const auto metricRow = [&](const int count) {
    const Rect row{content.x, y, content.width, metricRowHeight};
    renderer.drawRoundedRect(row.x, row.y, row.width, row.height, 1, 6, true, true, true, true, true);
    for (int index = 1; index < count; ++index) {
      const int x = row.x + row.width * index / count;
      renderer.drawLine(x, row.y + 5, x, row.y + row.height - 6);
    }
    y += metricRowHeight + sectionGap;
    return row;
  };
  const auto metricCell = [](const Rect& row, const int index, const int count) {
    const int x = row.x + row.width * index / count;
    return Rect{x, row.y, row.x + row.width * (index + 1) / count - x, row.height};
  };
  // Long streak labels need the wider two-column row.
  if (metricRows >= 1) {
    const Rect row = metricRow(3);
    char days[12];
    snprintf(days, sizeof(days), "%u", static_cast<unsigned>(summary.readingDays));
    char best[32] = "--";
    if (summary.bestDayKnown) {
      const std::string duration = ReadingCalendarRenderer::formatDuration(summary.bestDaySeconds);
      snprintf(best, sizeof(best), "%s - %u", duration.c_str(), static_cast<unsigned>(summary.bestDayOfMonth));
    }
    drawCalendarMetric(renderer, metricCell(row, 0, 3),
                       ReadingCalendarRenderer::formatDuration(summary.totalSeconds, summary.totalIsMinimum),
                       StrId::STR_STATS_MONTH_TOTAL);
    drawCalendarMetric(renderer, metricCell(row, 1, 3), days, StrId::STR_STATS_MONTH_DAYS);
    drawCalendarMetric(renderer, metricCell(row, 2, 3), best, StrId::STR_STATS_BEST_DAY);
  }
  if (metricRows >= 2) {
    const Rect row = metricRow(2);
    char streak[12];
    snprintf(streak, sizeof(streak), "%u", static_cast<unsigned>(model.snapshot().currentStreak));
    char longest[12];
    snprintf(longest, sizeof(longest), "%u", static_cast<unsigned>(summary.longestReadingStreak));
    drawCalendarMetric(renderer, metricCell(row, 0, 2), streak, StrId::STR_STATS_STREAK);
    drawCalendarMetric(renderer, metricCell(row, 1, 2), longest, StrId::STR_STATS_LONGEST_STREAK);
  }

  displayStrongSleepFrame();
}

void SleepActivity::renderCustomSleepScreen(const bool withBookStats) {
  // Check if we have a /.sleep (preferred) or /sleep directory
  const char* sleepDir = nullptr;
  auto dir = Storage.open("/.sleep");

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  // This takes priority over the /sleep folder.
  HalFile file;
  if (Storage.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file, true);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Loading: /sleep.bmp");
      renderBitmapSleepScreen(bitmap, false, withBookStats);
      file.close();
      if (dir) dir.close();
      return;
    }
    file.close();
  }

  if (dir && dir.isDirectory()) {
    sleepDir = "/.sleep";
  } else {
    dir = Storage.open("/sleep");
    if (dir && dir.isDirectory()) {
      sleepDir = "/sleep";
    }
  }

  if (sleepDir) {
    dir.close();
    const OverlayCandidate selected =
        directoryImageCandidate(sleepDir, [](const std::string& path) { return validateBmpOverlay(path); });
    if (selected.valid) {
      HalFile randFile;
      if (Storage.openFileForRead("SLP", selected.path, randFile)) {
        LOG_DBG("SLP", "Randomly loading: %s", selected.path.c_str());
        Bitmap bitmap(randFile, true);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap, false, withBookStats);
          randFile.close();
          return;
        }
        randFile.close();
      }
    }
  }
  if (dir) dir.close();

  if (withBookStats) {
    renderer.clearScreen();
    drawSleepBookStatsOverlay(renderer);
    displayStrongSleepFrame();
    return;
  }
  renderDefaultSleepScreen();
}

// Sleep is the last chance to remove accumulated charge before the panel is
// powered down. Use the strongest existing refresh here; normal UI and reader
// cadence remain unchanged.
void SleepActivity::renderDefaultSleepScreen() {
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  const bool isX3 = gpio.deviceIsX3();
  const bool hasBundledScreen =
      (isX3 && pageWidth == DEFAULT_SLEEP_X3_WIDTH && pageHeight == DEFAULT_SLEEP_X3_HEIGHT) ||
      (!isX3 && pageWidth == DEFAULT_SLEEP_X4_WIDTH && pageHeight == DEFAULT_SLEEP_X4_HEIGHT);
  if (hasBundledScreen) {
    if (isX3) {
      drawBundledDefaultScreen(renderer, DEFAULT_SLEEP_X3_WIDTH, DEFAULT_SLEEP_X3_HEIGHT, DefaultSleepX3Rows,
                               DefaultSleepX3Runs);
    } else {
      drawBundledDefaultScreen(renderer, DEFAULT_SLEEP_X4_WIDTH, DEFAULT_SLEEP_X4_HEIGHT, DefaultSleepX4Rows,
                               DefaultSleepX4Runs);
    }
  } else {
    // Keep the generated logo fallback for an unexpected orientation or panel.
    renderer.drawImage(Logo120, (pageWidth - 120) / 2, (pageHeight - 120) / 2, 120, 120);
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, tr(STR_CROSSPOINT), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, tr(STR_SLEEPING));
  }

  displayStrongSleepFrame();
}

void SleepActivity::renderTransparentSleepScreen() {
  SleepImageSelectionStore::Catalog catalog;
  const SleepImageSelectionStore::ImageTransform legacyTransform{
      SETTINGS.sleepScreenImageZoom, SETTINGS.sleepScreenImageOffsetX, SETTINGS.sleepScreenImageOffsetY};
  const SleepImageSelectionStore::CatalogStatus catalogStatus =
      SleepImageSelectionStore::loadCatalog(catalog, legacyTransform);
  if (catalogStatus != SleepImageSelectionStore::CatalogStatus::Ok || catalog.images.empty()) {
    LOG_ERR("SLP", "Sleep image catalog unavailable: status=%u count=%u", static_cast<unsigned>(catalogStatus),
            static_cast<unsigned>(catalog.images.size()));
    drawMoonOnCurrentFrame(renderer);
    displayStrongSleepFrame();
    return;
  }

  if (!SleepFrameStore::save(renderer)) {
    LOG_ERR("SLP", "Sleep overlay skipped: base frame could not be saved");
    drawMoonOnCurrentFrame(renderer);
    displayStrongSleepFrame();
    return;
  }

  bool restoreBase = false;
  while (!catalog.images.empty()) {
    const size_t selected = selectCatalogCandidate(catalog.images);
    SleepImageSelectionStore::ImageEntry candidate = std::move(catalog.images[selected]);
    catalog.images.erase(catalog.images.begin() + selected);

    if (restoreBase && !SleepFrameStore::load(display, false)) {
      wakeFrameReplayable_ = false;
      return renderDefaultSleepScreen();
    }

    LOG_DBG("SLP", "Rendering sleep overlay id=%u path=%s", static_cast<unsigned>(candidate.id),
            candidate.path.c_str());
    if (renderOverlayImage(candidate.path, renderer, candidate.transform)) {
      APP_STATE.pushRecentSleep(candidate.id);
      displayStrongSleepFrame();
      return;
    }

    LOG_ERR("SLP", "Sleep overlay failed id=%u path=%s", static_cast<unsigned>(candidate.id), candidate.path.c_str());
    restoreBase = true;
  }

  if (restoreBase && !SleepFrameStore::load(display, false)) {
    wakeFrameReplayable_ = false;
    return renderDefaultSleepScreen();
  }
  drawMoonOnCurrentFrame(renderer);
  displayStrongSleepFrame();
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap, const bool applyCoverFilter,
                                            const bool withBookStats) {
  const SleepImagePlacement placement =
      placeSleepImage(renderer, bitmap.getWidth(), bitmap.getHeight(), !applyCoverFilter);
  LOG_DBG("SLP", "bitmap %d x %d -> %d x %d at %d,%d zoom=%u", bitmap.getWidth(), bitmap.getHeight(), placement.width,
          placement.height, placement.x, placement.y,
          static_cast<unsigned>(applyCoverFilter ? SLEEP_IMAGE_DEFAULT_ZOOM : SETTINGS.sleepScreenImageZoom));
  renderer.clearScreen();

  const uint8_t filter =
      applyCoverFilter ? SETTINGS.sleepScreenCoverFilter : CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;
  const bool hasGreyscale = bitmap.hasGreyscale() && renderer.supportsStripGrayscale() &&
                            filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::NO_FILTER;

  renderer.drawBitmap(bitmap, placement.x, placement.y, placement.width, placement.height, 0, 0, true);

  const SleepBookSummary statsSummary = withBookStats ? loadSleepBookSummary() : SleepBookSummary{};
  const Rect statsCard = withBookStats ? drawSleepBookStatsOverlay(renderer, statsSummary) : Rect{};

  if (filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
    renderer.invertScreen();
  }

  const unsigned long grayscaleStartedAt = hasGreyscale ? millis() : 0;
  if (hasGreyscale) {
    LOG_INF("SLW", "sleep-frame grayscale begin base=full turn_off=1");
    applySleepGhostingTreatment();
    prepareStrongSleepRefresh();
    renderer.displayGrayscaleBase(HalDisplay::FULL_REFRESH);
    LOG_INF("SLW", "sleep-frame grayscale base complete elapsed_ms=%lu", millis() - grayscaleStartedAt);
  } else {
    displayStrongSleepFrame();
  }

  if (hasGreyscale) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, placement.x, placement.y, placement.width, placement.height, 0, 0, true);
    if (statsCard.width > 0 && statsCard.height > 0) {
      renderer.fillRect(statsCard.x, statsCard.y, statsCard.width, statsCard.height, true);
    }
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, placement.x, placement.y, placement.width, placement.height, 0, 0, true);
    if (statsCard.width > 0 && statsCard.height > 0) {
      renderer.fillRect(statsCard.x, statsCard.y, statsCard.width, statsCard.height, true);
    }
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer(TURN_OFF_SCREEN_AFTER_SLEEP_REFRESH);
    LOG_INF("SLW", "sleep-frame grayscale planes complete elapsed_ms=%lu", millis() - grayscaleStartedAt);
    renderer.setRenderMode(GfxRenderer::BW);

    // The panel keeps the grayscale image, but the shared framebuffer now
    // contains only the last gray plane. Rebuild the same composition as B/W
    // without touching the powered-down panel so normal wake can replay it.
    const bool rewound = bitmap.rewindToData() == BmpReaderError::Ok;
    renderer.clearScreen();
    const bool coverReady =
        rewound && renderer.drawBitmap(bitmap, placement.x, placement.y, placement.width, placement.height, 0, 0, true);
    if (withBookStats) drawSleepBookStatsOverlay(renderer, statsSummary);
    if (filter == CrossPointSettings::SLEEP_SCREEN_COVER_FILTER::INVERTED_BLACK_AND_WHITE) {
      renderer.invertScreen();
    }
    wakeFrameReplayable_ = coverReady;
    LOG_INF("SLW", "sleep-frame bw surrogate ready=%u", static_cast<unsigned>(wakeFrameReplayable_));
  }
}

void SleepActivity::renderCoverSleepScreen(const bool withBookStats) {
  if (APP_STATE.openEpubPath.empty()) {
    return renderDefaultSleepScreen();
  }

  std::string coverBmpPath;
  constexpr bool cropped = false;

  // Check if the current book is XTC, TXT, or EPUB
  if (FsHelpers::hasXtcExtension(APP_STATE.openEpubPath)) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      LOG_ERR("SLP", "Failed to load last XTC");
      return renderDefaultSleepScreen();
    }

    if (!lastXtc.generateCoverBmp()) {
      LOG_ERR("SLP", "Failed to generate XTC cover bmp");
      return renderDefaultSleepScreen();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (FsHelpers::hasTxtExtension(APP_STATE.openEpubPath) ||
             FsHelpers::hasMarkdownExtension(APP_STATE.openEpubPath)) {
    // Handle TXT file - looks for cover image in the same folder
    Txt lastTxt(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastTxt.load()) {
      LOG_ERR("SLP", "Failed to load last TXT");
      return renderDefaultSleepScreen();
    }

    if (!lastTxt.generateCoverBmp()) {
      LOG_ERR("SLP", "No cover image found for TXT file");
      return renderDefaultSleepScreen();
    }

    coverBmpPath = lastTxt.getCoverBmpPath();
  } else if (FsHelpers::hasEpubExtension(APP_STATE.openEpubPath)) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    coverBmpPath = lastEpub.getCoverBmpPath(cropped);
    const bool cachedCoverReady = Bitmap::inspectDerivedCache(coverBmpPath) == BitmapCacheState::Ready &&
                                  lastEpub.inspectSourceBinding() == Epub::SourceBindingStatus::Match;
    if (!cachedCoverReady) {
      // Skip loading CSS since we only need metadata when the validated
      // full-screen derived cover is absent.
      if (!lastEpub.load(true, true)) {
        LOG_ERR("SLP", "Failed to load last epub");
        return renderDefaultSleepScreen();
      }

      const Epub::ThumbnailRequest thumbnails{
          CrossPointSettings::needsSharedCoverThumbnail(SETTINGS.homeLayout, SETTINGS.libraryView),
          CrossPointSettings::needsCarouselCoverThumbnail(SETTINGS.homeLayout),
          renderer.getDisplayHeight() == 528,
      };
      if (!lastEpub.generateCoverBmp(cropped, thumbnails)) {
        LOG_ERR("SLP", "Failed to generate cover bmp");
        return renderDefaultSleepScreen();
      }
    }
  } else {
    return renderDefaultSleepScreen();
  }

  HalFile file;
  if (Storage.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      LOG_DBG("SLP", "Rendering sleep cover: %s", coverBmpPath.c_str());
      renderBitmapSleepScreen(bitmap, true, withBookStats);
      return;
    }
  }

  return renderDefaultSleepScreen();
}

void SleepActivity::renderLastScreenSleepScreen() {
  const auto pageHeight = renderer.getScreenHeight();
  renderer.drawImage(MoonIcon, 0, pageHeight - MOONICON_HEIGHT, MOONICON_WIDTH, MOONICON_HEIGHT);
  displayStrongSleepFrame();
}

void SleepActivity::renderBlankSleepScreen() {
  renderer.clearScreen();
  displayStrongSleepFrame();
}
