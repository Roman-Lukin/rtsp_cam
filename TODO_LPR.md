# TODO: Rozpoznawanie Tablic Rejestracyjnych (LPR) na ESP32-P4

## Cel
Offline rozpoznawanie polskich tablic rejestracyjnych bez zewnętrznych serwerów.
Przetwarzanie nie musi być real-time (~1-2 FPS wystarczy).

---

## Faza 1: Przygotowanie środowiska (1-2 dni)

- [x] Dodać ESP-DL do `idf_component.yml`:
  ```yaml
  espressif/esp-dl: "^3.2.2"
  ```
- [ ] Sprawdzić dostępną pamięć PSRAM po uruchomieniu kamery i RTSP
- [ ] Utworzyć strukturę katalogów (dostosowaną do ESP-DL 3.x):
  ```
  main/
    lpr/
      lpr.h
      lpr.c
      detection.c    # detekcja tablicy (post-processing)
      ocr.c          # rozpoznawanie znaków
  models/
      plate_detect.espdl  # Format FlatBuffers (ESP-DL 3.x)
      plate_ocr.espdl
  ```

---

## Faza 2: Detekcja ruchu - filtr wstępny (2-3 dni)

### Opcja A: Sprzętowa (ZALECANA dla ESP32-P4)
- [x] Wykorzystanie wektorów ruchu (Motion Vectors) z encodera H.264
- [x] ESP32-P4 posiada sprzętowy encoder H.264, który może zwracać MV dla makrobloków 16x16
- [x] Zaleta: Prawie zerowe obciążenie CPU (dane "za darmo" przy kompresji wideo)
- [x] API: `esp_h264_enc_hw_set_mv_pkt()` w komponencie `espressif/esp_h264`
  - Zaimplementowano "dirty hack" w `main.c` aby dostać się do uchwytu HW z poziomu `esp_capture`.

### Opcja B: Programowa (Frame Difference)
- [ ] Bufor 2 klatek do porównania (grayscale, downscaled)
- [ ] Tylko jeśli Opcja A okaże się niemożliwa w obecnej konfiguracji pipeline'u

---

## Faza 3: Model detekcji tablicy (1-2 tygodnie)

### Architektura
- [ ] **YOLO11n** lub **ESPDet-Pico** (oficjalnie wspierane w przykładach ESP-DL 3.x)
- [ ] ESP-DL 3.x posiada zoptymalizowane operatory dla tych architektur

### Trening i Konwersja (Nowy workflow)
- [ ] Zebrać dataset polskich tablic (min. 1000 zdjęć)
- [ ] Trening w PyTorch (export do ONNX)
- [ ] **Kwantyzacja i Export**: Użyć narzędzia **`esp-ppq`** (zastępuje stare `esp-dl-tools`)
  - `esp-ppq` pozwala na kalibrację kwantyzacji (INT8) i eksport do formatu `.espdl`
  - Obsługa "Dual Core Scheduling" (automatyczny podział pracy na 2 rdzenie)

### Specyfikacja modelu
- Wejście: 320x240 lub 160x120 RGB
- Format: `.espdl` (FlatBuffers)
- Czas inference: target < 100ms (dzięki akceleratorowi AI w P4)

---

## Faza 4: Model OCR - rozpoznawanie znaków (1-2 tygodnie)

### Opcja A: LPRNet (end-to-end)
- [ ] Wytrenować LPRNet w PyTorch
- [ ] Konwersja przez `esp-ppq` do `.espdl`

### Opcja B: Segmentacja + CNN (Klasyczna)
- [ ] Prostsza w debugowaniu, ale może być wolniejsza
- [ ] CNN dla pojedynczego znaku (20x20 px) - bardzo szybki na P4

### Polskie tablice - format
- [ ] Obsługa formatów: XX 12345, XX 1234A, XXX 12345
- [ ] Znaki: A-Z (bez Q), 0-9

---

## Faza 5: Integracja z projektem (3-5 dni)

### API
```c
// main/lpr/lpr.h
#include "dl_tool.hpp" // ESP-DL 3.x headers

typedef struct {
    char plate_text[16];
    float confidence;
    int bbox[4]; // x, y, w, h
    int64_t timestamp;
} lpr_result_t;

// ... (reszta API bez zmian)
```

### Integracja
- [ ] Wykorzystanie **Dual Core Scheduling** (ESP-DL 3.x robi to automatycznie dla ciężkich warstw)
- [ ] Użycie **Static Memory Planner** z ESP-DL 3.x do optymalizacji zużycia SRAM/PSRAM

---

## Faza 6: Optymalizacja (3-5 dni)

- [ ] **H.264 Motion Vectors**: Sprawdzenie czy ruch występuje w strefie zainteresowania (ROI)
- [ ] **Zero-copy**: Wykorzystanie formatu `.espdl` który wspiera ładowanie bez kopiowania
- [ ] **SIMD**: Weryfikacja czy operatory używają instrukcji wektorowych ESP32-P4

---

## Zasoby i linki

### ESP-DL 3.x
- Dokumentacja: https://github.com/espressif/esp-dl/tree/master/esp-dl
- Narzędzie konwersji: **https://github.com/espressif/esp-ppq** (Kluczowe!)
- Przykłady detekcji: https://github.com/espressif/esp-dl/tree/master/examples/human_face_detect (można zaadaptować do tablic)

### Datasety
- https://medialab.put.poznan.pl/ (polskie datasety)

---

## Szacowany czas całości: 4-6 tygodni

## Wymagania sprzętowe (ESP32-P4)
| Zasób | Dostępne | Potrzebne | Uwagi |
|-------|----------|-----------|-------|
| PSRAM | 32 MB | ~4-6 MB | ESP-DL 3.x lepiej zarządza pamięcią |
| Flash | 16 MB | ~2-3 MB | Modele .espdl są bardziej kompaktowe |
| AI Accel| Tak | - | Wykorzystywane przez instrukcje wektorowe |

## Priorytety
1. 🔴 Detekcja ruchu (H.264 MV - test feasibility)
2. 🔴 Trening modelu detekcji (YOLO11n -> esp-ppq)
3. 🔴 Model OCR
4. 🟡 Integracja
