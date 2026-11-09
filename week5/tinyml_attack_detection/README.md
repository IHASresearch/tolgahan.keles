# TinyML ile Paket Düşürme (Drop) Atağı Tespiti

İki kart arasındaki Wi-Fi bağlantısında paket düşürme atağını, ESP32 üzerinde çalışan küçük bir sinir ağıyla tespit ediyoruz.

## Akış

1. **Veri toplama:** `sender` atak simülasyonu yapar, `receiver` ağın durumunu ölçer, `logger.py` bunu CSV'ye yazar.
2. **Eğitim:** `train.py` bu CSV ile küçük bir MLP eğitir ve `detector/model.h` dosyasını üretir.
3. **Tespit:** `detector` aynı ölçümleri yapar ve modeli ESP32 üzerinde çalıştırır.

## Dosyalar

| Dosya | Görevi |
|---|---|
| `sender/sender.ino` | Client (ESP32 veya ESP8266, aynı kod). Saniyede 100 UDP paketi yollar. 30 sn normal, 30 sn atak döngüsü yapar. Atakta paketlerin bir kısmını göndermez ve TX gücünü düşürür. Her paket etiketini (0/1) taşır. |
| `receiver/receiver.ino` | Server (ESP32). AP kurar, paketleri dinler. Her 1 sn için kayıp, paket aralığı, RSSI, retry gibi değerleri tek bir CSV satırı olarak yazar. 500 normal + 500 atak satırda durur. |
| `logger.py` | Receiver'ın seri port çıktısını CSV dosyasına kaydeder. |
| `drop_dataset.csv` | Toplanan veri seti (1000 satır). |
| `train.py` | MLP eğitir (10 özellik → 16 → 8 → 1), test sonucunu yazar, ağırlıkları `detector/model.h` dosyasına C dizisi olarak kaydeder. |
| `detector/detector.ino` | Server'ın tespit sürümü. Her pencerede modeli çalıştırır, tahmini gerçek etiketle karşılaştırıp doğruluğu yazar. |
| `detector/model.h` | `train.py`'nin ürettiği model (elle değiştirilmez). |

## Kullanım

Python script'lerinde ayarlar dosyanın üstündeki **PARAMETRELER** kısmındadır (port, dosya yolu, epoch).

```powershell
# 1) Veri toplama: sender'ı ve receiver'ı yükle, sonra
python logger.py

# 2) Eğitim
python train.py

# 3) Tespit: sender'ı ve detector'ı yükle
arduino-cli compile --fqbn esp32:esp32:esp32 detector
arduino-cli upload -p COM9 --fqbn esp32:esp32:esp32 detector
arduino-cli monitor -p COM9 -c baudrate=115200
```

Sender'ı ESP8266'ya yüklemek için sadece kart tipini değiştirmek yeterli:

```powershell
arduino-cli upload -p COM6 --fqbn esp8266:esp8266:nodemcuv2 sender
```

Receiver ve detector sadece ESP32'de çalışır (ESP32'nin sniffer API'sini kullanıyorlar).

Mevcut veriyle test doğruluğu **%98.5**. Model 321 parametreden oluşur (1.3 KB).
