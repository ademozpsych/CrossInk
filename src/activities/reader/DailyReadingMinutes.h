#pragma once
#include <array>
#include <cstdint>

#include "ReadingStatsUtils.h"

// Gun basina okunan dakika sayisi. CrossInk'in global_stats.bin dosyasi bir gun
// icin yalnizca "okundu / okunmadi" biti tutar; okuma kartindaki isi haritasinin
// yogunluk tonlari icin gunluk sureye ihtiyac var.
//
// Ayri bir dosyada saklanir (/.crosspoint/daily_minutes.bin) — boylece ust
// akistaki global_stats.bin bicimi degismez ve firmware geri alinirsa mevcut
// istatistikler bozulmaz. Dosya yoksa kart ikili (okudu/okumadi) haritaya duser.
//
// Dosya duzeni (v1, 737 bayt):
//   [0]       surum bayti
//   [1..4]    anchorDay (en son kayitli gun; 2000-01-01'den beri gun sayisi)
//   [5..734]  730 gun, her biri 1 bayt: o gun okunan dakika (255'te doyar)
//   [735,736] ayrilmis
//
// Yapi 736 bayt tuttugu icin yigit uzerinde olusturulmaz; hem kayit hem okuma
// yollari nesneyi obek uzerinde ayirir (bkz. CLAUDE.md kaynak kurallari 1 ve 10).
struct DailyReadingMinutes {
  static constexpr size_t DAYS = READING_HISTORY_DAYS;  // 730
  static constexpr uint8_t FILE_VERSION = 1;
  static constexpr size_t FILE_SIZE = 5 + DAYS + 2;

  uint32_t anchorDay = 0;
  std::array<uint8_t, DAYS> minutes{};

  // Bir okuma araligini gunlere dagitarak dosyaya isler. Gece yarisini asan
  // oturumlar dogru gunlere bolunur. Ayirma basarisiz olursa sessizce cikar;
  // gunluk dakika kaydi istege bagli bir sustur, okuma akisini bloklamaz.
  static void recordAndSave(const ReadingStatsDateTime& localStart, uint32_t seconds);

  // Dosyayi obek uzerinde okur. Dosya yoksa bos kayit dondurur (nullptr degil,
  // ancak ayirma basarisiz olursa nullptr).
  static bool loadInto(DailyReadingMinutes& out);

  void save() const;
  void recordReadingSpan(const ReadingStatsDateTime& localStart, uint32_t seconds);

  // dayIndex icin kayitli dakika; kayit yoksa 0.
  uint8_t minutesForDayIndex(uint32_t dayIndex) const;

  // En az bir gun icin dakika kaydi var mi? Yoksa kart ikili haritaya duser.
  bool hasAnyData() const;

private:
  void addMinutes(uint32_t dayIndex, uint32_t addedMinutes);
  // Dosya ilk kez olusurken, global gecmis tek bir gunden ibaretse toplam
  // sureyi o gune yazar (bu firmware oncesi okumalari da haritaya katar).
  static void seedFromGlobalStatsIfSingleDay(DailyReadingMinutes& daily);
};
