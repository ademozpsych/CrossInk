#pragma once
#include <string>

#include "activities/reader/DailyReadingMinutes.h"
#include "activities/reader/GlobalReadingStats.h"

class GfxRenderer;

// Uyku ekrani icin "okuma karti": tek bakista okuma aliskanligini gosteren,
// paylasilabilir bir ozet. Ust kisimda seri sayaci ve haftalik sure, ortada
// GitHub tarzi gunluk isi haritasi, altta toplamlar ve okunan kitap.
//
// Cizim siyah-uzerine-beyaz mantigiyla degil, normal (beyaz zemin, siyah muhur)
// yapilir; SleepActivity cizim bittikten sonra renderer.invertScreen() cagirir,
// boylece kart koyu zeminde beyaz olarak gorunur. Gri tonlar bu yuzden ters
// yonde secilir: seyrek dither = koyu, yogun dither = acik.
namespace ReadingCardScreen {

struct BookLine {
  std::string title;
  std::string author;
  float progressPercent = -1.0f;  // 0..100, bilinmiyorsa negatif
};

// daily bos ise (dosya yoksa ya da hic dakika kaydi yoksa) isi haritasi
// globalStats'in gunluk okuma bitlerinden ikili olarak cizilir.
void render(const GfxRenderer& renderer, const GlobalReadingStats& globalStats, const DailyReadingMinutes& daily,
            const BookLine& book);

}  // namespace ReadingCardScreen
