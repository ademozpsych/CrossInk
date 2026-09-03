#include "ReadingCardScreen.h"

#include <GfxRenderer.h>
#include <HalClock.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "CrossPointSettings.h"
#include "fontIds.h"

namespace ReadingCardScreen {
namespace {

// Ust satirdaki tarih, cihazin Ayarlar > Sistem'de secilen tarih bicimini ve
// ayiracini kullanir; boylece kart ile ana ekran basligi ayni gorunur.
char dateSeparatorChar() {
  switch (SETTINGS.dateSeparator) {
    case CrossPointSettings::DATE_SEPARATOR_PERIOD:
      return '.';
    case CrossPointSettings::DATE_SEPARATOR_HYPHEN:
      return '-';
    case CrossPointSettings::DATE_SEPARATOR_SLASH:
    default:
      return '/';
  }
}

bool formatTodayDate(char* buf, const size_t len) {
  if (!halClock.isAvailable()) {
    return false;
  }
  return halClock.formatDate(buf, len, SETTINGS.clockUtcOffsetQ, static_cast<HalClock::DateFormat>(SETTINGS.dateFormat),
                             dateSeparatorChar());
}

// Bosluklu listeden 1 tabanli ogeyi alir (ay ve gun adlari icin ortak).
void pickListItem(const char* list, const uint8_t oneBasedIndex, char* buf, const size_t len) {
  uint8_t index = 1;
  const char* start = list;
  while (*start && index < oneBasedIndex) {
    if (*start == ' ') {
      ++index;
    }
    ++start;
  }
  size_t n = 0;
  while (start[n] && start[n] != ' ' && n + 1 < len) {
    buf[n] = start[n];
    ++n;
  }
  buf[n] = '\0';
}

// Kisa ay adi, arayuz dilinden (STR_CARD_MONTHS_SHORT: bosluklu 12 ad).
void localizedShortMonth(const uint8_t month, char* buf, const size_t len) {
  pickListItem(tr(STR_CARD_MONTHS_SHORT), month, buf, len);
  if (buf[0] == '\0') {
    snprintf(buf, len, "%u", static_cast<unsigned>(month));
  }
}

// Haftanin gunu adi (Pazartesi = 0), arayuz dilinden.
void localizedWeekday(const uint8_t mondayBasedIndex, char* buf, const size_t len) {
  pickListItem(tr(STR_CARD_WEEKDAYS), static_cast<uint8_t>(mondayBasedIndex + 1), buf, len);
}

// Metni en fazla iki satira sarar; ikinci satir sigmazsa "..." ile kirpilir.
// Kitap adlari icin: tam ad gorunsun, ekran tasmasin.
void drawWrappedTwoLines(const GfxRenderer& renderer, const int fontId, const int x, int& y, const int maxWidth,
                         const char* text) {
  const int lineH = renderer.getLineHeight(fontId);
  if (renderer.getTextWidth(fontId, text) <= maxWidth) {
    renderer.drawText(fontId, x, y, text, true);
    y += lineH;
    return;
  }

  // Ilk satir icin siginan en uzun kelime sinirini bul.
  const size_t total = strlen(text);
  size_t breakAt = 0;
  for (size_t i = 0; i < total; ++i) {
    if (text[i] != ' ') {
      continue;
    }
    char candidate[128];
    const size_t n = std::min(i, sizeof(candidate) - 1);
    memcpy(candidate, text, n);
    candidate[n] = '\0';
    if (renderer.getTextWidth(fontId, candidate) <= maxWidth) {
      breakAt = i;
    } else {
      break;
    }
  }

  if (breakAt == 0) {
    // Tek uzun kelime: kirparak tek satir ciz.
    const std::string clipped = renderer.truncatedText(fontId, text, maxWidth);
    renderer.drawText(fontId, x, y, clipped.c_str(), true);
    y += lineH;
    return;
  }

  char first[128];
  const size_t n = std::min(breakAt, sizeof(first) - 1);
  memcpy(first, text, n);
  first[n] = '\0';
  renderer.drawText(fontId, x, y, first, true);
  y += lineH;

  const std::string second = renderer.truncatedText(fontId, text + breakAt + 1, maxWidth);
  renderer.drawText(fontId, x, y, second.c_str(), true);
  y += lineH;
}

// Isi haritasi olcusu. Genislik calisma zamaninda hesaplanir; hucre ve bosluk
// sabit kalir, sutun sayisi ekrana sigacak sekilde kirpilir.
constexpr int kCell = 13;
constexpr int kGap = 3;
constexpr int kMaxWeeks = 26;
constexpr int kRows = 7;

// Yogunluk esikleri (dakika). Beyaz hucre nadir kalsin diye ust esik yuksek.
constexpr uint8_t kLevel2Minutes = 35;
constexpr uint8_t kLevel3Minutes = 75;

// Ekran ters cevrilecegi icin ton secimi ters yonde yapilir:
// seyrek dither (az siyah) -> ters cevrilince koyu; yogun dither -> acik.
// Boylece 0..3 seviyeleri kartta sonik olarak sonuk -> parlak siralanir.
void fillCell(const GfxRenderer& renderer, const int x, const int y, const int level) {
  switch (level) {
    case 0: {
      // Bos gun: hucrenin ortasinda kucuk bir isaret. Tam dolgu dither burada
      // veri gibi okunuyordu; nokta izgara yapisini korurken sessiz kaliyor.
      const int dot = 3;
      const int offset = (kCell - dot) / 2;
      renderer.fillRect(x + offset, y + offset, dot, dot, true);
      break;
    }
    case 1:
      renderer.fillRectDither(x, y, kCell, kCell, Color::LightGray);
      break;
    case 2:
      renderer.fillRectDither(x, y, kCell, kCell, Color::DarkGray);
      break;
    default:
      renderer.fillRect(x, y, kCell, kCell, true);
      break;
  }
}

int levelForMinutes(const uint8_t minutes) {
  if (minutes == 0) return 0;
  if (minutes < kLevel2Minutes) return 1;
  if (minutes < kLevel3Minutes) return 2;
  return 3;
}

bool historyBitSet(const GlobalReadingStats& stats, const uint32_t dayIndex) {
  if (stats.readingHistoryAnchorDay == 0 || dayIndex > stats.readingHistoryAnchorDay) {
    return false;
  }
  const uint32_t delta = stats.readingHistoryAnchorDay - dayIndex;
  if (delta >= READING_HISTORY_DAYS) {
    return false;
  }
  return (stats.readingHistoryBits[delta / 8] >> (delta % 8)) & 1;
}

// Etiketlerde harf araligi: buyuk harfli kucuk etiketler bu sayede
// "story" gorunumune yaklasiyor.
int drawTracked(const GfxRenderer& renderer, const int fontId, int x, const int y, const char* text,
                const int spacing) {
  char ch[5] = {};
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    int len = 1;
    if ((*p & 0xF8) == 0xF0) {
      len = 4;
    } else if ((*p & 0xF0) == 0xE0) {
      len = 3;
    } else if ((*p & 0xE0) == 0xC0) {
      len = 2;
    }
    for (int i = 0; i < len; ++i) ch[i] = static_cast<char>(p[i]);
    ch[len] = '\0';
    if (ch[0] == ' ') {
      // Tek basina olculen bosluk sifir genislik donuyor; kelime arasini
      // fontun bosluk genisligiyle ac ki "BU HAFTA" birlesmesin.
      x += renderer.getSpaceWidth(fontId) + spacing * 2;
    } else {
      renderer.drawText(fontId, x, y, ch, true);
      x += renderer.getTextWidth(fontId, ch) + spacing;
    }
    p += len;
  }
  return x;
}

int trackedWidth(const GfxRenderer& renderer, const int fontId, const char* text, const int spacing) {
  int width = 0;
  int glyphs = 0;
  char ch[5] = {};
  const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    int len = 1;
    if ((*p & 0xF8) == 0xF0) {
      len = 4;
    } else if ((*p & 0xF0) == 0xE0) {
      len = 3;
    } else if ((*p & 0xE0) == 0xC0) {
      len = 2;
    }
    for (int i = 0; i < len; ++i) ch[i] = static_cast<char>(p[i]);
    ch[len] = '\0';
    width += ch[0] == ' ' ? renderer.getSpaceWidth(fontId) + spacing : renderer.getTextWidth(fontId, ch);
    ++glyphs;
    p += len;
  }
  return width + spacing * std::max(0, glyphs - 1);
}

void drawRightText(const GfxRenderer& renderer, const int fontId, const int rightEdge, const int y, const char* text) {
  renderer.drawText(fontId, rightEdge - renderer.getTextWidth(fontId, text), y, text, true);
}

void formatHoursMinutes(const uint32_t seconds, char* buf, const size_t len) {
  const uint32_t minutes = seconds / 60;
  snprintf(buf, len, "%luh %02lum", static_cast<unsigned long>(minutes / 60),
           static_cast<unsigned long>(minutes % 60));
}

// Binlik ayraci ile sayi (5870 -> "5.870"). Kart tipografisinde okunurluk icin.
void formatGrouped(const uint32_t value, char* buf, const size_t len) {
  char raw[16];
  snprintf(raw, sizeof(raw), "%lu", static_cast<unsigned long>(value));
  const int digits = static_cast<int>(strlen(raw));
  size_t out = 0;
  for (int i = 0; i < digits && out + 2 < len; ++i) {
    if (i > 0 && (digits - i) % 3 == 0) {
      buf[out++] = '.';
    }
    buf[out++] = raw[i];
  }
  buf[out] = '\0';
}

}  // namespace

void render(const GfxRenderer& renderer, const GlobalReadingStats& globalStats, const DailyReadingMinutes& daily,
            const BookLine& book) {
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int margin = std::max(24, screenW / 13);
  const int contentW = screenW - margin * 2;
  const int rightEdge = screenW - margin;

  renderer.clearScreen();

  ReadingStatsDateTime now;
  const bool hasNow = getCurrentLocalReadingStatsDateTime(now);
  const uint32_t todayIndex = hasNow ? readingStatsDayIndex(now.date) : globalStats.readingHistoryAnchorDay;
  const bool useMinutes = daily.hasAnyData();

  // --- ust etiket ----------------------------------------------------------
  int y = std::max(28, screenH / 22);
  char kicker[96];
  char dateText[32];
  if (formatTodayDate(dateText, sizeof(dateText))) {
    char weekday[24] = "";
    if (hasNow) {
      localizedWeekday(readingStatsDayOfWeekIndex(now.date), weekday, sizeof(weekday));
    }
    // Ust satir yalnizca gun ve tarih; "OKUMA" etiketi istenmedi.
    if (weekday[0] != '\0') {
      snprintf(kicker, sizeof(kicker), "%s · %s", weekday, dateText);
    } else {
      snprintf(kicker, sizeof(kicker), "%s", dateText);
    }
  } else {
    // Saat yoksa tarih yerine kartin adi kalir ki satir bos gorunmesin.
    snprintf(kicker, sizeof(kicker), "%s", tr(STR_READING_CARD));
  }
  drawTracked(renderer, UI_10_FONT_ID, margin, y, kicker, 2);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 8;
  renderer.fillRect(margin, y, contentW, 1, true);

  // --- kahraman: gecerli seri ----------------------------------------------
  // Seri: gunluk dakika kaydi varsa oradan sayilir, boylece isi haritasiyla ayni
  // kaynagi kullanir. Kayit yoksa global istatistiklerin gun bitlerine duser.
  uint16_t streak = 0;
  if (useMinutes) {
    // Bugun henuz okunmadiysa seri dunden geriye sayilir; boylece sabah
    // uyandirmada dun aksam kurulan seri sifirlanmis gibi gorunmez.
    uint32_t cursor = todayIndex;
    if (daily.minutesForDayIndex(cursor) == 0 && cursor > 0) {
      --cursor;
    }
    while (daily.minutesForDayIndex(cursor) > 0) {
      ++streak;
      if (cursor == 0) {
        break;
      }
      --cursor;
    }
  } else {
    const ReadingStatsDate* todayPtr = hasNow ? &now.date : nullptr;
    streak = computeReadingHistoryCurrentStreak(globalStats.readingHistoryAnchorDay, globalStats.readingHistoryBits,
                                                todayPtr);
  }

  char streakText[8];
  snprintf(streakText, sizeof(streakText), "%u", static_cast<unsigned>(streak));
  const int heroY = y + 14;
  renderer.drawText(HERO_64_FONT_ID, margin, heroY, streakText, true);
  const int heroW = renderer.getTextWidth(HERO_64_FONT_ID, streakText);
  const int heroH = renderer.getLineHeight(HERO_64_FONT_ID);
  renderer.drawText(UI_12_FONT_ID, margin + heroW + 8, heroY + heroH - renderer.getLineHeight(UI_12_FONT_ID) - 6,
                    tr(STR_CARD_DAY_UNIT), true);

  // --- sag ust: bu haftaki sure --------------------------------------------
  uint32_t weekMinutes = 0;
  for (uint32_t back = 0; back < 7 && todayIndex >= back; ++back) {
    const uint32_t dayIndex = todayIndex - back;
    if (useMinutes) {
      weekMinutes += daily.minutesForDayIndex(dayIndex);
    }
  }
  if (useMinutes) {
    char weekText[24];
    snprintf(weekText, sizeof(weekText), "%luh %02lum", static_cast<unsigned long>(weekMinutes / 60),
             static_cast<unsigned long>(weekMinutes % 60));
    drawRightText(renderer, LEXENDDECA_16_FONT_ID, rightEdge, heroY + 8, weekText);
    const int labelW = trackedWidth(renderer, UI_10_FONT_ID, tr(STR_CARD_THIS_WEEK), 2);
    drawTracked(renderer, UI_10_FONT_ID, rightEdge - labelW,
                heroY + 8 + renderer.getLineHeight(LEXENDDECA_16_FONT_ID) + 4, tr(STR_CARD_THIS_WEEK), 2);
  }

  y = heroY + heroH + 6;
  drawTracked(renderer, UI_10_FONT_ID, margin, y, tr(STR_CARD_STREAK_LABEL), 2);
  y += renderer.getLineHeight(UI_10_FONT_ID) + 26;

  // --- isi haritasi ---------------------------------------------------------
  const int weeks = std::min(kMaxWeeks, (contentW + kGap) / (kCell + kGap));
  const int gridW = weeks * (kCell + kGap) - kGap;
  const int gridH = kRows * (kCell + kGap) - kGap;
  const int gridX = margin;
  const int monthLabelY = y;
  const int gridY = y + renderer.getLineHeight(UI_10_FONT_ID) + 4;

  // Izgaranin ilk sutunu, en eski haftanin pazartesisinde baslar.
  const uint8_t todayDow = hasNow ? readingStatsDayOfWeekIndex(now.date) : 0;
  const uint32_t firstDayIndex = todayIndex - todayDow - static_cast<uint32_t>(7 * (weeks - 1));

  uint16_t activeDays = 0;
  uint8_t lastMonthLabeled = 0;
  for (int col = 0; col < weeks; ++col) {
    for (int row = 0; row < kRows; ++row) {
      const uint32_t dayIndex = firstDayIndex + static_cast<uint32_t>(col * 7 + row);
      if (dayIndex > todayIndex) {
        continue;
      }
      const int cx = gridX + col * (kCell + kGap);
      const int cy = gridY + row * (kCell + kGap);
      int level;
      if (useMinutes) {
        level = levelForMinutes(daily.minutesForDayIndex(dayIndex));
      } else {
        level = historyBitSet(globalStats, dayIndex) ? 3 : 0;
      }
      if (level > 0) {
        ++activeDays;
      }
      fillCell(renderer, cx, cy, level);

      // Ay etiketi: sutunun ilk gunu yeni bir ayin ilk haftasindaysa.
      if (row == 0) {
        ReadingStatsDate date;
        if (readingStatsDateFromDayIndex(dayIndex, date) && date.day <= 7 && date.month != lastMonthLabeled) {
          lastMonthLabeled = date.month;
          char monthToken[16];
          localizedShortMonth(date.month, monthToken, sizeof(monthToken));
          renderer.drawText(UI_10_FONT_ID, cx, monthLabelY, monthToken, true);
        }
      }
    }
  }

  // --- gosterge ve not ------------------------------------------------------
  const int legendY = gridY + gridH + 14;
  const int legendCell = 10;
  int lx = margin;
  renderer.drawText(UI_10_FONT_ID, lx, legendY - 2, tr(STR_CARD_LESS), true);
  lx += renderer.getTextWidth(UI_10_FONT_ID, tr(STR_CARD_LESS)) + 8;
  for (int level = 0; level < 4; ++level) {
    const int drawLevel = useMinutes ? level : (level == 0 ? 0 : 3);
    switch (drawLevel) {
      case 0:
        // Okunmamis gun: ici bos kare. Haritadaki noktadan daha belirgin,
        // boylece gostergede dort durum acikca sayilabiliyor.
        renderer.drawRect(lx, legendY, legendCell, legendCell, true);
        break;
      case 1:
        renderer.fillRectDither(lx, legendY, legendCell, legendCell, Color::LightGray);
        break;
      case 2:
        renderer.fillRectDither(lx, legendY, legendCell, legendCell, Color::DarkGray);
        break;
      default:
        renderer.fillRect(lx, legendY, legendCell, legendCell, true);
        break;
    }
    lx += legendCell + 3;
  }
  renderer.drawText(UI_10_FONT_ID, lx + 5, legendY - 2, tr(STR_CARD_MORE), true);

  // Not, gostergenin sagina yazilir. Sigmazsa hafta bilgisi dusurulur; ay
  // etiketleri zaten araligi gosterdigi icin bu kayip onemsiz.
  const int legendRight = lx + 5 + renderer.getTextWidth(UI_10_FONT_ID, tr(STR_CARD_MORE));
  char activeText[24];
  char weeksText[24];
  char note[56];
  snprintf(activeText, sizeof(activeText), tr(STR_CARD_ACTIVE_DAYS), static_cast<int>(activeDays));
  snprintf(weeksText, sizeof(weeksText), tr(STR_CARD_LAST_WEEKS), weeks);
  snprintf(note, sizeof(note), "%s · %s", activeText, weeksText);
  if (renderer.getTextWidth(UI_10_FONT_ID, note) > rightEdge - legendRight - 12) {
    snprintf(note, sizeof(note), "%s", activeText);
  }
  if (renderer.getTextWidth(UI_10_FONT_ID, note) <= rightEdge - legendRight - 12) {
    drawRightText(renderer, UI_10_FONT_ID, rightEdge, legendY - 2, note);
  }

  // --- toplamlar ------------------------------------------------------------
  int rowY = legendY + legendCell + 20;
  renderer.fillRect(margin, rowY, contentW, 1, true);
  const int valueY = rowY + 18;
  const int labelY = valueY + renderer.getLineHeight(LEXENDDECA_16_FONT_ID) + 6;

  char totalText[24];
  formatHoursMinutes(globalStats.totalReadingSeconds, totalText, sizeof(totalText));
  char pagesText[24];
  formatGrouped(globalStats.totalPagesTurned, pagesText, sizeof(pagesText));
  char booksText[16];
  snprintf(booksText, sizeof(booksText), "%lu", static_cast<unsigned long>(globalStats.completedBooks));

  const struct {
    int centerX;
    const char* value;
    const char* label;
  } columns[3] = {
      {margin + contentW / 6, totalText, tr(STR_CARD_TOTAL_TIME)},
      {margin + contentW / 2, pagesText, tr(STR_CARD_PAGES)},
      {margin + contentW * 5 / 6, booksText, tr(STR_CARD_BOOKS)},
  };
  for (const auto& column : columns) {
    renderer.drawText(LEXENDDECA_16_FONT_ID,
                      column.centerX - renderer.getTextWidth(LEXENDDECA_16_FONT_ID, column.value) / 2, valueY,
                      column.value, true);
    const int labelW = trackedWidth(renderer, UI_10_FONT_ID, column.label, 1);
    drawTracked(renderer, UI_10_FONT_ID, column.centerX - labelW / 2, labelY, column.label, 1);
  }

  rowY = labelY + renderer.getLineHeight(UI_10_FONT_ID) + 14;
  renderer.fillRect(margin, rowY, contentW, 1, true);

  // --- su an okunan kitap ---------------------------------------------------
  if (book.title.empty()) {
    return;
  }
  int by = rowY + 22;
  drawTracked(renderer, UI_10_FONT_ID, margin, by, tr(STR_CARD_NOW_READING), 1);
  by += renderer.getLineHeight(UI_10_FONT_ID) + 10;

  drawWrappedTwoLines(renderer, LEXENDDECA_16_FONT_ID, margin, by, contentW, book.title.c_str());
  by += 4;

  // Yazar solda, yuzde ayni satirin saginda: kitap adi iki satira sarildiginda
  // bile ilerleme cubugu ekranin icinde kalir.
  const bool hasProgress = book.progressPercent >= 0.0f;
  char pctText[12] = "";
  int pctW = 0;
  if (hasProgress) {
    snprintf(pctText, sizeof(pctText), "%%%d", static_cast<int>(book.progressPercent + 0.5f));
    pctW = renderer.getTextWidth(UI_12_FONT_ID, pctText);
    drawRightText(renderer, UI_12_FONT_ID, rightEdge, by, pctText);
  }
  if (!book.author.empty()) {
    const int authorMaxW = contentW - (hasProgress ? pctW + 16 : 0);
    const std::string author = renderer.truncatedText(UI_12_FONT_ID, book.author.c_str(), authorMaxW);
    renderer.drawText(UI_12_FONT_ID, margin, by, author.c_str(), true);
  }
  by += renderer.getLineHeight(UI_12_FONT_ID) + 10;

  if (hasProgress && by + 8 <= screenH) {
    const int barH = 7;
    const int filled = static_cast<int>(contentW * std::min(1.0f, book.progressPercent / 100.0f));
    renderer.drawRect(margin, by, contentW, barH, true);
    if (filled > 0) {
      renderer.fillRect(margin, by, filled, barH, true);
    }
  }
}

}  // namespace ReadingCardScreen
