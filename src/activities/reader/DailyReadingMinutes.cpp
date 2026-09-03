#include "DailyReadingMinutes.h"

#include "GlobalReadingStats.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace {
constexpr char DAILY_MINUTES_PATH[] = "/.crosspoint/daily_minutes.bin";
constexpr char DAILY_MINUTES_TMP_PATH[] = "/.crosspoint/daily_minutes.bin.tmp";

// Gunler ileri kaydiginda diziyi kaydir: index 0 her zaman anchorDay'dir.
void shiftOlder(std::array<uint8_t, DailyReadingMinutes::DAYS>& minutes, const uint32_t shiftDays) {
  if (shiftDays == 0) {
    return;
  }
  if (shiftDays >= DailyReadingMinutes::DAYS) {
    minutes.fill(0);
    return;
  }
  for (size_t i = DailyReadingMinutes::DAYS; i-- > shiftDays;) {
    minutes[i] = minutes[i - shiftDays];
  }
  std::fill(minutes.begin(), minutes.begin() + shiftDays, 0);
}
}  // namespace

bool DailyReadingMinutes::loadInto(DailyReadingMinutes& out) {
  out.anchorDay = 0;
  out.minutes.fill(0);

  FsFile f;
  if (!Storage.openFileForRead("DMIN", DAILY_MINUTES_PATH, f)) {
    // Dosya henuz yok: temiz baslangic, hata degil.
    return true;
  }

  uint8_t header[5] = {};
  const int headerRead = f.read(header, sizeof(header));
  if (headerRead != static_cast<int>(sizeof(header)) || header[0] != FILE_VERSION) {
    f.close();
    LOG_DBG("DMIN", "Daily minutes missing or unsupported version, starting fresh");
    return true;
  }

  out.anchorDay = static_cast<uint32_t>(header[1]) | (static_cast<uint32_t>(header[2]) << 8) |
                  (static_cast<uint32_t>(header[3]) << 16) | (static_cast<uint32_t>(header[4]) << 24);

  // Gunluk diziyi dogrudan yapinin icine oku; ayrica tampon ayirmiyoruz.
  const int bodyRead = f.read(out.minutes.data(), DAYS);
  f.close();
  if (bodyRead < 0) {
    LOG_ERR("DMIN", "Daily minutes body read failed, treating as empty");
    out.anchorDay = 0;
    out.minutes.fill(0);
  }
  return true;
}

void DailyReadingMinutes::save() const {
  if (Storage.exists(DAILY_MINUTES_TMP_PATH)) {
    Storage.remove(DAILY_MINUTES_TMP_PATH);
  }

  FsFile f;
  if (!Storage.openFileForWrite("DMIN", DAILY_MINUTES_TMP_PATH, f)) {
    LOG_ERR("DMIN", "Could not open daily minutes temp file for write");
    return;
  }

  uint8_t header[5];
  header[0] = FILE_VERSION;
  header[1] = anchorDay & 0xFF;
  header[2] = (anchorDay >> 8) & 0xFF;
  header[3] = (anchorDay >> 16) & 0xFF;
  header[4] = (anchorDay >> 24) & 0xFF;
  const uint8_t reserved[2] = {0, 0};

  const size_t headerWritten = f.write(header, sizeof(header));
  const size_t bodyWritten = f.write(minutes.data(), DAYS);
  const size_t tailWritten = f.write(reserved, sizeof(reserved));
  f.flush();
  const bool synced = f.sync();
  f.close();

  if (headerWritten != sizeof(header) || bodyWritten != DAYS || tailWritten != sizeof(reserved) || !synced) {
    LOG_ERR("DMIN", "Short write for daily minutes, keeping previous file");
    Storage.remove(DAILY_MINUTES_TMP_PATH);
    return;
  }

  Storage.remove(DAILY_MINUTES_PATH);
  if (!Storage.rename(DAILY_MINUTES_TMP_PATH, DAILY_MINUTES_PATH)) {
    LOG_ERR("DMIN", "Could not move daily minutes into place");
    Storage.remove(DAILY_MINUTES_TMP_PATH);
  }
}

void DailyReadingMinutes::addMinutes(const uint32_t dayIndex, const uint32_t addedMinutes) {
  if (addedMinutes == 0) {
    return;
  }

  if (anchorDay == 0) {
    anchorDay = dayIndex;
    minutes.fill(0);
  } else if (dayIndex > anchorDay) {
    shiftOlder(minutes, dayIndex - anchorDay);
    anchorDay = dayIndex;
  }

  const uint32_t delta = anchorDay - dayIndex;
  if (delta >= DAYS) {
    return;
  }

  const uint32_t total = static_cast<uint32_t>(minutes[delta]) + addedMinutes;
  minutes[delta] = static_cast<uint8_t>(total > 255 ? 255 : total);
}

void DailyReadingMinutes::recordReadingSpan(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  if (!localStart.isValid() || seconds == 0) {
    return;
  }

  ReadingStatsDateTime cursor = localStart;
  uint32_t remaining = seconds;
  while (remaining > 0) {
    const uint32_t secondsUntilMidnight =
        (24u * 3600u) - (static_cast<uint32_t>(cursor.hour) * 3600u + static_cast<uint32_t>(cursor.minute) * 60u +
                         static_cast<uint32_t>(cursor.second));
    const uint32_t segment = remaining < secondsUntilMidnight ? remaining : secondsUntilMidnight;
    addMinutes(readingStatsDayIndex(cursor.date), (segment + 30u) / 60u);
    remaining -= segment;
    addSecondsToReadingStatsDateTime(cursor, segment);
  }
}

void DailyReadingMinutes::recordAndSave(const ReadingStatsDateTime& localStart, const uint32_t seconds) {
  if (!localStart.isValid() || seconds == 0) {
    return;
  }

  // 736 baytlik yapi okuyucu cikis yolundaki paylasilan gorev yigitina
  // konmamali; obek uzerinde ayirip hemen birakiyoruz.
  auto daily = makeUniqueNoThrow<DailyReadingMinutes>();
  if (!daily) {
    LOG_ERR("DMIN", "Could not allocate daily minutes buffer, skipping this session");
    return;
  }

  loadInto(*daily);

  // Bu firmware'den onceki okuma suresi gunluk dosyada yok ama global
  // istatistiklerde var. Gecmis yalnizca tek bir gune aitse (cihaz yeni ya da
  // ilk gunu) o gunun dakikasini global toplama esitleyerek "bu hafta" ile
  // "toplam sure" arasindaki tutarsizligi kapatiyoruz. Birden fazla gun varsa
  // dagilimi bilemeyiz; o zaman dokunmuyoruz.
  seedFromGlobalStatsIfSingleDay(*daily);

  daily->recordReadingSpan(localStart, seconds);
  daily->save();
}

void DailyReadingMinutes::seedFromGlobalStatsIfSingleDay(DailyReadingMinutes& daily) {
  // Not: cagiran taraf bu oturumu bellekteki global toplama ekledi ama dosyaya
  // henuz yazmadi; buradaki load() bu oturumu icermez, cift sayim olmaz.
  const GlobalReadingStats global = GlobalReadingStats::load();
  if (global.readingHistoryAnchorDay == 0 || global.totalReadingSeconds == 0) {
    return;
  }

  uint32_t activeDays = 0;
  uint32_t onlyDayDelta = 0;
  for (uint32_t i = 0; i < READING_HISTORY_DAYS; ++i) {
    if ((global.readingHistoryBits[i / 8] >> (i % 8)) & 1) {
      ++activeDays;
      onlyDayDelta = i;
      if (activeDays > 1) {
        return;
      }
    }
  }
  if (activeDays != 1) {
    return;
  }

  const uint32_t dayIndex = global.readingHistoryAnchorDay - onlyDayDelta;
  const uint32_t globalMinutes = std::min<uint32_t>(255u, (global.totalReadingSeconds + 30u) / 60u);

  if (daily.anchorDay == 0) {
    daily.anchorDay = dayIndex;
    daily.minutes.fill(0);
  } else if (dayIndex > daily.anchorDay) {
    shiftOlder(daily.minutes, dayIndex - daily.anchorDay);
    daily.anchorDay = dayIndex;
  }
  if (dayIndex > daily.anchorDay) {
    return;
  }
  const uint32_t delta = daily.anchorDay - dayIndex;
  if (delta >= DAYS) {
    return;
  }

  // Esitleme: gunluk kayit global toplamin gerisindeyse yukari cek; asla dusurme.
  if (daily.minutes[delta] < globalMinutes) {
    LOG_INF("DMIN", "Daily minutes seeded from global stats: day=%lu %u -> %lu min",
            static_cast<unsigned long>(dayIndex), static_cast<unsigned>(daily.minutes[delta]),
            static_cast<unsigned long>(globalMinutes));
    daily.minutes[delta] = static_cast<uint8_t>(globalMinutes);
  }
}

uint8_t DailyReadingMinutes::minutesForDayIndex(const uint32_t dayIndex) const {
  if (anchorDay == 0 || dayIndex > anchorDay) {
    return 0;
  }
  const uint32_t delta = anchorDay - dayIndex;
  return delta < DAYS ? minutes[delta] : 0;
}

bool DailyReadingMinutes::hasAnyData() const {
  if (anchorDay == 0) {
    return false;
  }
  for (const uint8_t value : minutes) {
    if (value != 0) {
      return true;
    }
  }
  return false;
}
