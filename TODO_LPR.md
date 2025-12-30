# TODO: Rozpoznawanie Tablic Rejestracyjnych (LPR) na ESP32-P4

## Cel
Offline rozpoznawanie polskich tablic rejestracyjnych bez zewnętrznych serwerów.
Przetwarzanie nie musi być real-time (~1-2 FPS wystarczy).

---

## Faza 1: Przygotowanie środowiska (1-2 dni)

- [ ] Dodać ESP-DL do `idf_component.yml`:
  ```yaml
  espressif/esp-dl: "^2.0.0"
  ```
- [ ] Sprawdzić dostępną pamięć PSRAM po uruchomieniu kamery i RTSP
- [ ] Utworzyć strukturę katalogów:
  ```
  main/
    lpr/
      lpr.h
      lpr.c
      detection.c    # detekcja tablicy
      ocr.c          # rozpoznawanie znaków
  models/
      plate_detect.espdl
      plate_ocr.espdl
  ```

---

## Faza 2: Detekcja ruchu - filtr wstępny (2-3 dni)

- [ ] Implementacja prostej detekcji ruchu (frame difference)
- [ ] Bufor 2 klatek do porównania (grayscale, downscaled 320x240)
- [ ] Próg czułości (konfigurowalny)
- [ ] Uruchamianie LPR tylko gdy wykryto ruch (oszczędność CPU)

```c
// Pseudo-struktura
typedef struct {
    uint8_t *prev_frame;      // poprzednia klatka (grayscale)
    uint8_t *curr_frame;      // aktualna klatka
    int motion_threshold;     // próg detekcji
    bool motion_detected;
} motion_detector_t;
```

---

## Faza 3: Model detekcji tablicy (1-2 tygodnie)

### Opcja A: Gotowy model (szybsza)
- [ ] Pobrać pre-trained YOLO-Nano lub SSD-MobileNet dla tablic
- [ ] Źródła:
  - https://github.com/openalpr/openalpr (modele)
  - https://github.com/sergiomsilva/alpr-unconstrained
  - https://platerecognizer.com (sample models)

### Opcja B: Własny trening (dokładniejsza)
- [ ] Zebrać dataset polskich tablic (min. 1000 zdjęć)
- [ ] Oznakować tablice (LabelImg/CVAT)
- [ ] Wytrenować YOLO-Nano/SSD w PyTorch/TensorFlow
- [ ] Eksport do ONNX

### Konwersja modelu
- [ ] Zainstalować ESP-DL tools:
  ```bash
  pip install esp-dl-tools
  ```
- [ ] Konwersja ONNX -> ESP-DL format
- [ ] Kwantyzacja INT8 (kluczowa dla wydajności!)
- [ ] Test na PC przed wgraniem na ESP32

### Specyfikacja modelu detekcji
- Wejście: 320x240 lub 160x120 RGB/Grayscale
- Wyjście: Bounding box (x, y, w, h) + confidence
- Rozmiar: max 1 MB (INT8)
- Czas inference: target < 500ms

---

## Faza 4: Model OCR - rozpoznawanie znaków (1-2 tygodnie)

### Opcja A: LPRNet (end-to-end)
- [ ] Pobrać/wytrenować LPRNet
- [ ] Wejście: wycięta tablica 140x32 px
- [ ] Wyjście: string znaków

### Opcja B: CRNN + CTC
- [ ] Model CRNN dla sekwencji znaków
- [ ] CTC decoder na ESP32

### Opcja C: Segmentacja + CNN per znak (najprostsza)
- [ ] Segmentacja znaków (kontury/projekcja)
- [ ] Klasyfikator CNN dla pojedynczego znaku (36 klas: A-Z, 0-9)
- [ ] Łatwiejsza do debugowania

### Polskie tablice - format
- [ ] Obsługa formatów: XX 12345, XX 1234A, XXX 12345
- [ ] Znaki: A-Z (bez Q), 0-9
- [ ] Wyróżnik województwa (opcjonalnie)

### Specyfikacja modelu OCR
- Wejście: 140x32 grayscale (wycięta tablica)
- Wyjście: string max 8 znaków
- Rozmiar: max 2 MB (INT8)
- Czas inference: target < 300ms

---

## Faza 5: Integracja z projektem (3-5 dni)

### API
```c
// main/lpr/lpr.h
typedef struct {
    char plate_text[16];      // rozpoznany tekst "WA 12345"
    int confidence;           // pewność 0-100%
    int bbox_x, bbox_y;       // pozycja tablicy
    int bbox_w, bbox_h;
    int64_t timestamp;        // czas detekcji
} lpr_result_t;

typedef void (*lpr_callback_t)(const lpr_result_t *result, void *user_data);

esp_err_t lpr_init(const lpr_config_t *config);
esp_err_t lpr_set_callback(lpr_callback_t cb, void *user_data);
esp_err_t lpr_process_frame(const uint8_t *rgb_frame, int width, int height);
void lpr_deinit(void);
```

### Integracja z capture task
- [ ] Dodać opcjonalne przetwarzanie LPR w `capture_task()`
- [ ] Osobny task dla LPR (niższy priorytet niż RTSP)
- [ ] Kolejka klatek do przetworzenia
- [ ] Nie blokować streamingu RTSP

### Wyniki
- [ ] Callback z rozpoznaną tablicą
- [ ] Logowanie do konsoli
- [ ] Opcjonalnie: zapis do NVS/SPIFFS
- [ ] Opcjonalnie: wysyłanie MQTT/HTTP

---

## Faza 6: Optymalizacja (3-5 dni)

- [ ] Profilowanie czasu inference
- [ ] Optymalizacja pamięci (współdzielenie buforów)
- [ ] Wykorzystanie DSP/SIMD ESP32-P4
- [ ] Testowanie z różnym oświetleniem
- [ ] Testowanie z różnymi kątami/odległościami

---

## Faza 7: Testy i walidacja (3-5 dni)

- [ ] Test accuracy na zbiorze walidacyjnym
- [ ] Test false positive rate
- [ ] Test w warunkach rzeczywistych (parking, brama)
- [ ] Dokumentacja API
- [ ] Przykładowe użycie

---

## Zasoby i linki

### ESP-DL
- Dokumentacja: https://github.com/espressif/esp-dl
- Przykłady: https://github.com/espressif/esp-dl/tree/master/examples
- Model Zoo: https://github.com/espressif/esp-dl/tree/master/models

### Datasety polskich tablic
- https://github.com/opencv/opencv/tree/master/samples/data (ogólne)
- https://medialab.put.poznan.pl/ (polskie datasety)
- Własne zdjęcia z kamery projektu

### Narzędzia
- LabelImg: https://github.com/tzutalin/labelImg (oznaczanie)
- Netron: https://netron.app/ (wizualizacja modeli)
- ESP-DL Converter: w pakiecie esp-dl

---

## Szacowany czas całości: 4-6 tygodni

## Wymagania sprzętowe
| Zasób | Dostępne | Potrzebne |
|-------|----------|-----------|
| PSRAM | 32 MB | ~4-6 MB |
| Flash | 16 MB | ~3-4 MB (modele) |
| CPU | 400 MHz | OK |

## Priorytety
1. 🔴 Detekcja ruchu (filtr wstępny)
2. 🔴 Model detekcji tablicy
3. 🔴 Model OCR
4. 🟡 Integracja z RTSP
5. 🟢 Optymalizacja
6. 🟢 Testy

---

*Ostatnia aktualizacja: 30.12.2024*
