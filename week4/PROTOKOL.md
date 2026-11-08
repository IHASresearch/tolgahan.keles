# Kanal Atlamalı Çift Yönlü Haberleşme Protokolü

İHA ile yer istasyonu arasında, nRF24L01 modülleriyle 5 saniyede bir kanal değiştiren, AES-128 ile şifreli, çift yönlü bir haberleşme protokolü. Tasarımın odak noktası, bağlantı koptuğunda sistemin kendi kendine toparlanmasıdır.

## 1. Roller

| Cihaz | Rol | Kod | Görev |
|---|---|---|---|
| İHA | **Master** | `sender/sender.ino` | Zamanın sahibidir. Kendi saatine göre atlar, hiçbir koşulda durmaz ya da bloklanmaz. |
| Yer istasyonu | **Slave** | `receiver/receiver.ino` | Master'ı bulur, onun saatine kilitlenir ve kilidi korur. |

Temel fikir şudur: **Master asla durmaz, senkronu bulmak Slave'in işidir.** Arama, kilitlenme ve kopma yönetimi gibi bütün karmaşıklık Slave tarafında toplanır.

## 2. Parametreler

İki cihazda da birebir aynı olmalıdır.

| Parametre | Değer | Açıklama |
|---|---|---|
| Hop tablosu | `{75, 25, 49, 81, 50, 76, 80, 77}` | WiFi 1/6/11 kanallarının arasındaki boşluklar |
| Slot süresi | 5 s | Her slotta bir kanal kullanılır |
| Guard | ±20 ms | Master, slot sınırına 20 ms'den yakınken gönderim yapmaz |
| Paket aralığı | 20 ms | Master saniyede ~50 paket gönderir |
| Adres | `"UAV01"` | Tek adres; ACK'ler de bu adres üzerinden döner |
| Veri hızı / CRC | 1 Mbps / CRC-16 | |
| Auto-ACK retry | 3 deneme, 750 µs aralık | En kötü durumda ~4 ms, guard'ın içinde kalır |

Slave'e özgü zamanlamalar:

| Parametre | Değer | Gerekçe |
|---|---|---|
| Tarama bekleme süresi | 25 ms/kanal | Paket aralığından uzun; Master o kanaldaysa en az bir paket yakalanır. 8 kanallık tam tur ~200 ms sürer. |
| HOLDOVER eşiği | 500 ms paketsiz | ~25 ardışık paket kaybı gerçek bir kesinti demektir |
| SCAN eşiği | 15 s (3 slot) paketsiz | Art arda iki bozuk kanalı tolere eder. ESP32 kristali ~40 ppm saptığından 15 s'deki kayma ~0.6 ms'dir, guard'ın çok altındadır. |

Master'a özgü zamanlama: Son 500 ms içinde ACK alınmadıysa link **LINK_DOWN** sayılır.

## 3. Zaman ve kanal seçimi

- Master'ın zamanı `esp_timer_get_time()` ile alınır, yani açılıştan beri geçen mikrosaniye.
- Slave kendi sistem saatini değiştirmez. Onun yerine bir `offset` tutar:
  `masterZamanı = esp_timer_get_time() + offset`
- Her iki taraf da kanalı aynı formülle hesaplar:

```
slot  = masterZamanı / 5 000 000
kanal = CHANNELS[slot % 8]
```

Slave her geçerli pakette `offset = paket.timeUs - yerelZaman` ile offset'i günceller, böylece kristal kayması sürekli düzeltilir. Paket gelmediği sürede son offset ile atlamaya devam eder.

## 4. Çift yönlü veri: ACK payload

nRF24'ün **auto-ACK + ACK payload** özelliği kullanılır:

1. Master bir DATA paketi gönderir.
2. Slave'in donanımı paketi alır almaz otomatik olarak ACK döner. ACK'in içinde Slave'in FIFO'ya önceden yüklediği 32 baytlık şifreli veri taşınır.
3. Master'ın `write()` çağrısı `true` dönerse paket ulaşmıştır ve varsa ACK payload okunur.
4. Slave her paket aldıktan sonra bir sonraki ACK payload'unu FIFO'ya yükler.

Bu yöntemde yer→İHA verisi, bir sonraki İHA paketiyle yani birkaç on milisaniye gecikmeyle gider. Ek bir zamanlama ya da TX/RX değişimi gerekmez.

Uygulamada dikkat edilen noktalar:

- **ACK payload ne zaman silinir:** nRF24, gönderdiği bir ACK payload'unu FIFO'dan ancak bir sonraki (yeni) paket geldiğinde siler. Bu yüzden Slave her gelen pakette tam olarak bir yeni mesaj yükler. "FIFO boşsa yükle" mantığı, ACK'lerin yarısının boş gitmesine yol açar.
- **Kanal değişimi:** RF24 kütüphanesinde `stopListening()`, ACK payload etkinken TX FIFO'yu temizler. Slave her kanal değişiminden sonra gönderilmemiş mesajı aynı seq ile yeniden yükler.
- **Gönderim fonksiyonu:** Master, `RF24::write()` yerine kendi `radioSend()` fonksiyonunu kullanır. RF24 1.6.2'de `FAILURE_HANDLING` açıkken `write()` içindeki kurtarma mantığı hatalıdır (`if (doRecovery)` pointer'ı kontrol eder) ve sonsuz döngüye girebilir. `radioSend()` durum register'ını yalnızca okuyarak bekler, 10 ms zaman aşımı vardır. Bayrakları okurken aynı anda temizlemek (`whatHappened()`), o sırada kalkan bir bayrağın kaybolmasına neden olur.

## 5. Paket formatları

Her iki paket de 32 bayttır ve AES-128 ECB ile 2 blok halinde şifrelenir.

**İHA → Yer (DATA):**

```
| type (1) | sessionId (2) | seq (4) | timeUs (8) | payload (17) |
```

| Alan | Açıklama |
|---|---|
| `type` | `PKT_DATA = 1` |
| `sessionId` | Master'ın her açılışta rastgele ürettiği oturum kimliği. Değişirse Master yeniden başlamış demektir. |
| `seq` | Her pakette 1 artan sayaç. Kayıp paket istatistiği için kullanılır. |
| `timeUs` | Master'ın gönderim anındaki zamanı (µs) |
| `payload` | Telemetri verisi |

**Yer → İHA (ACK payload):**

```
| type (1) | seq (4) | payload (27) |
```

| Alan | Açıklama |
|---|---|
| `type` | `PKT_ACK = 2` |
| `seq` | Her ACK payload'unda 1 artan sayaç |
| `payload` | Komut verisi |

## 6. Durum makineleri

### Master (İHA)

```
RUN ─┬─ LINK_UP   : son 500 ms içinde ACK alındı
     └─ LINK_DOWN : ACK alınmadı
```

Master'ın tek durumu **RUN**'dır. Link durumu yalnızca bilgi amaçlıdır; Master bağlantı kopsa da atlamaya ve göndermeye devam eder.

### Slave (yer istasyonu)

```
           geçerli paket (offset'i al)
  SCAN ──────────────────────────────────► LOCKED ◄─────────┐
   ▲                                       │    ▲            │ paket geldi
   │                                       │    │            │
   │                          500 ms       │    └── yeni sessionId → offset'i yeniden al
   │                          paketsiz     ▼                 │
   │         15 s paketsiz              HOLDOVER ────────────┘
   └────────────────────────────────────┘
```

| Durum | Kanal davranışı | Çıkış koşulu |
|---|---|---|
| **SCAN** | Kanalları 25 ms'lik dilimlerle sırayla tarar | Geçerli DATA paketi gelir → offset ve sessionId alınır → **LOCKED** |
| **LOCKED** | Master saatine göre atlar, her paketle offset'i günceller | 500 ms paket gelmezse → **HOLDOVER** |
| **HOLDOVER** | Son offset ile atlamaya devam eder | Paket gelirse → **LOCKED**; 15 s paket gelmezse → **SCAN** |

Herhangi bir durumda farklı bir `sessionId` gelirse Master yeniden başlamış demektir. Slave offset'i hemen yeniden alır ve **LOCKED** durumuna geçer.

## 7. Kopma senaryoları

| Senaryo | Slave davranışı | Toparlanma süresi |
|---|---|---|
| Bir kanal tamamen bozuk (WiFi, parazit) | LOCKED → HOLDOVER → LOCKED | Sonraki slot (≤5 s) |
| Kısa süreli menzil kaybı (<15 s) | HOLDOVER, doğru kanallarda beklemeye devam eder | Link döner dönmez |
| Uzun süreli menzil kaybı (>15 s) | SCAN | Link döndükten sonra <1 s |
| İHA yeniden başladı | Master'ın saati sıfırlandığı için kanal sırası kayar. Slave tesadüfen aynı kanaldaysa yeni sessionId'yi görüp hemen kilitlenir, değilse HOLDOVER → SCAN üzerinden bulur. | Anında ile ~15 s arası |
| Yer istasyonu yeniden başladı | SCAN ile başlar | <1 s |
| Asimetrik link (yer İHA'yı duyuyor, İHA ACK'leri duymuyor) | Slave LOCKED kalır, telemetri akmaya devam eder | — (Master LINK_DOWN raporlar) |

## 8. Seri port çıktıları

Her iki cihaz da saniyede bir özet basar. Paket istatistikleri hem son 1 saniye hem de açılıştan beri toplam olarak verilir.

- **Master:** link durumu, kanal, son alınan yer istasyonu verisi
  - `UAV->GS`: gönderilen ve ACK alınan paket sayısı, kayıp oranı
  - `GS->UAV`: ACK payload ile gelen ve kaybolan (seq boşluklarından hesaplanan) mesaj sayısı, kayıp oranı
- **Slave:** durum, kanal, sessionId, son alınan İHA verisi
  - `UAV->GS`: alınan ve kaybolan (seq boşluklarından hesaplanan) paket sayısı, kayıp oranı. Kesinti sırasında kaçırılan paketler de kayba dahildir.

Slave ayrıca her kanal atlamasında biten slotun özetini basar (`Slot done on ch 76: rx 248, lost 2`). Böylece hangi kanalın bozuk olduğu doğrudan görülür.

Durum değişimleri ve kanal atlamaları da anında yazdırılır.

> Modüller masada birbirine yakınken `PA_LEVEL = RF24_PA_LOW` kullanılır. `RF24_PA_MAX` yakın mesafede alıcıyı doyurur ve paket kaybına yol açar. Menzil testinde `RF24_PA_MAX`'e geçilir.
