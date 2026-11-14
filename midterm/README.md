# Midterm: İHA Görüntülerinden Askeri Araç Tespiti ve Model Quantization

İHA ya da yer kamerasından alınan görüntülerde **askeri araç** ve **personel** tespit eden bir YOLO modeli geliştirdik. Ardından modeli gömülü veya uçuş bilgisayarında gerçek zamanlı çalışabilmesi için **TensorRT FP16** formatına quantize ettik.

Projenin adımları:

1. Açık Roboflow datasetlerini indirmek, analiz etmek ve temizlemek
2. Birden fazla dataseti sızıntısız şekilde tek bir datasette birleştirmek
3. YOLO26'yı bu veriyle fine-tune etmek (Ultralytics Platform)
4. Modeli TensorRT FP16'ya quantize edip doğruluk ve hız kaybını ölçmek
5. Kamera veya video üzerinde canlı tespit yapmak

Çalışmanın tamamı ve tüm sonuçlar raporda: [docs/Midterm_Raporu_Tolgahan_Keles.pdf](docs/Midterm_Raporu_Tolgahan_Keles.pdf) (Word hali aynı klasörde). Bu README raporun özetidir; sayılar rapordaki test seti sonuçlarıdır.

---

## Klasör yapısı

```
midterm/
├── README.md
├── src/
│   ├── common.py              # ortak yollar + YOLO etiket yardımcıları
│   ├── download_datasets.py   # 1) Roboflow'dan ek datasetleri indir
│   ├── data_cleaning.py       # 2) tek dataseti analiz et / temizle   (v1 -> dataset_clean)
│   ├── merge_datasets.py      # 3) 5 dataseti birleştir               (v2 -> dataset_merged)
│   ├── export_model.py        # 4) TensorRT FP16 quantization + .pt / .engine karşılaştırması
│   └── detect.py              # 5) kamera / video üzerinde canlı tespit
├── models/
│   ├── exp-2.pt               # YOLO26n, 4 sınıf (dataset_clean)
│   ├── exp-2.onnx             # export ara çıktısı (FP32)
│   ├── exp-2.fp16.onnx        # ONNX FP16
│   ├── exp-2.engine           # TensorRT FP16 (bu PC'nin GPU'suna özel)
│   ├── exp-3.pt               # YOLO26n, 2 sınıf (dataset_merged)
│   └── exp-4.pt               # YOLO26l, 2 sınıf (dataset_merged)
├── videos/
│   └── test_1.mp4             # demo videosu
├── docs/                      # ara sınav raporu
│   ├── Midterm_Raporu_Tolgahan_Keles.pdf
│   └── Midterm_Raporu_Tolgahan_Keles.docx
├── data/                      # (git'te yok) datasetler
│   ├── dataset_clean/
│   ├── dataset_merged/
│   └── dataset_merged.zip     # Ultralytics Platform'a yüklenen arşiv
└── runs/                      # (git'te yok) değerlendirme ve kamera çıktıları
```

---

## Kurulum

Repo kökündeki ortak ortam kullanılıyor:

```powershell
python -m venv .venv
.venv\Scripts\activate
pip install torch torchvision --index-url https://download.pytorch.org/whl/cu128   # NVIDIA GPU
pip install -r requirements.txt
pip install tensorrt                     # sadece .engine export / çalıştırma için
```

Aşağıdaki tüm komutlar `midterm/` klasöründen çalıştırılır:

```powershell
cd midterm
```

---

## Kullanım

### Hazır modelle tespit (en hızlı başlangıç)

```powershell
python src/detect.py                                       # webcam (0), varsayılan: exp-2.engine
python src/detect.py --source videos/test_1.mp4 --save     # video, çıktı runs/camera/ altına
python src/detect.py --weights models/exp-4.pt             # en doğru model (2 sınıf)
python src/detect.py --source rtsp://192.168.1.10/stream   # İHA'nın video akışı
```

Tuşlar: `q`/`ESC` çıkış, `s` ekran görüntüsü, `+`/`-` güven eşiği.

> `exp-2.engine` bu bilgisayarın GPU'su ve TensorRT sürümü için derlendi. Başka bir makinede `.pt` dosyasını kullanın ya da `export_model.py` ile yeniden export edin.

### Verinin baştan hazırlanması

Ham Roboflow datasetlerinin dosya adları çok uzun olduğundan (Windows `MAX_PATH` sınırı), bu datasetler kısa yollarda tutuluyor. Yollar [src/common.py](src/common.py) içinde tanımlı:

- Ana dataset (`huyen-dinh/military-vehicle-rstvv` v2): `C:\military-vehicle-dataset`
- Ek datasetler: `C:\datasets\<isim>`

```powershell
$env:ROBOFLOW_API_KEY="..."
python src/download_datasets.py            # ek datasetleri indir

python src/data_cleaning.py analyze        # ana dataset için rapor
python src/data_cleaning.py clean          # -> data/dataset_clean   (v1, 4 sınıf)
python src/merge_datasets.py --zip         # -> data/dataset_merged  (v2, 2 sınıf) + zip
```

### Eğitim

Modeller [Ultralytics Platform](https://platform.ultralytics.com)'da eğitildi: `dataset_merged.zip` yüklendi, `yolo26n`/`yolo26l` ile 100 epoch fine-tune yapıldı. Yerelde aynı eğitim şöyle yapılabilir:

```powershell
yolo detect train model=yolo26n.pt data=data/dataset_merged/data.yaml epochs=100 imgsz=640
```

### Quantization (TensorRT FP16)

```powershell
python src/export_model.py                                   # models/exp-2.pt -> exp-2.engine
python src/export_model.py --weights models/exp-4.pt         # büyük model
python src/export_model.py --weights models/exp-2.pt --skip-export   # sadece karşılaştır
```

Script modelin sınıf sayısına bakarak doğru test setini kendi seçer: 4 sınıf için `dataset_clean`, 2 sınıf için `dataset_merged`. 2 sınıflı modellerde ayrıca sadece havadan çekilmiş görüntülerden oluşan `test_aerial` alt kümesi de ölçülür.

---

## Ne yaptık?

### 1. Veri: v1, tek dataset temizliği (`data_cleaning.py`)

> Bu adım ön çalışmadır; raporda yalnızca burada bulunan veri kalitesi sorunları anlatılıyor, `dataset_clean` ve onunla eğitilen `exp-2` modeli raporda yer almıyor.

İlk olarak tek bir Roboflow dataseti (`military-vehicle-rstvv`) kullandık. Analiz sırasında şu sorunları bulduk:

- **Sınıf adı hatası ve az örnekli sınıf:** `amoured-vehicle` yazım hatası vardı. `artillery` sınıfında ise sadece 49 kutu olduğu için bu sınıf tek başına öğrenilemezdi. İkisi `armored-vehicle` altında birleştirildi.
- **Veri sızıntısı:** Roboflow augment kopyaları (`isim_jpg.rf.<hash>.jpg`) farklı split'lere dağılmıştı, yani eğitimdeki görüntünün kopyası test setinde de bulunuyordu. Kopyalar kaynak görüntüye göre gruplanıp aynı split'e alındı.
- **Birebir tekrar ve bozuk dosyalar:** MD5 ile tespit edilip atıldı. Hatalı veya sınır dışı kutular da düzeltildi.
- **Yeniden bölme:** Her sınıf her split'te aynı oranda bulunacak şekilde katmanlı 80/15/5 bölme yapıldı.

Sonuç (`dataset_clean`, 4 sınıf: armored-vehicle, person, tank, truck):

| split | görüntü | armored-vehicle | person | tank | truck |
|---|---:|---:|---:|---:|---:|
| train | 8153 | 1752 | 4304 | 6506 | 908 |
| valid | 1529 | 372 | 897 | 1208 | 206 |
| test | 509 | 145 | 268 | 416 | 67 |

### 2. Veri: v2, beş datasetin birleştirilmesi (`merge_datasets.py`)

v1 datasetinin görüntüleri çoğunlukla yerden çekilmiş. İHA senaryosu için **havadan çekilmiş** görüntüler gerekiyordu. Bu yüzden 4 ek dataset daha ekledik (`gzhet`, `roman`, `view6`, `mamad`; son ikisi havadan çekilmiş). Bu datasetlerin sınıf adları birbirinden farklı olduğu için sınıfları **military-vehicle** ve **person** olarak 2 sınıfa indirdik. Sivil araç, uçak, helikopter gibi sınıflar atıldı.

Birleştirmede şu adımlar uygulandı:

- **Datasetler arası tekrar tespiti:** Aynı görüntü birden fazla datasette bulunabiliyordu. MD5 ve perceptual hash (dHash) ile kontrol edildi. Hamming ≤ 2 olanlar tekrar sayıldı ve **5809 kopya** atıldı. Hamming ≤ 6 olanlar ise silinmedi ama aynı gruba alındı.
- **Video kareleri:** Aynı videodan gelen ardışık kareler, dosya adına bakılarak aynı gruba konuldu. Böylece neredeyse aynı karelerin biri train'e, diğeri test'e düşmedi.
- **Dataset bazında katmanlı bölme:** Her kaynak (özellikle havadan çekilmiş `view6` ve `mamad`) her split'te oranlı olarak bulunuyor.
- `name_map.csv` yeni dosya adlarını orijinal adlarla eşliyor. `duplicates.csv` hangi dosyanın hangisinin kopyası olarak atıldığını tutuyor.

Sonuç (`dataset_merged`, toplam 36 997 görüntü):

| split | görüntü | military-vehicle | person |
|---|---:|---:|---:|
| train | 29603 | 43062 | 24858 |
| valid | 5547 | 8784 | 3671 |
| test | 1847 | 3338 | 1226 |

Kaynak dağılımı: gzhet 12786, huyen 9434, roman 6561, mamad 4389, view6 3827.

### 3. Fine-tuning

COCO üzerinde önceden eğitilmiş YOLO26n ve YOLO26l modelleri `dataset_merged` ile Ultralytics Platform'da fine-tune edildi. İkisinde de `imgsz=640`, 100 epoch, MuSGD ve mozaik augmentation (son 10 epoch kapalı) kullanıldı. Doğrulama mAP50-95 değeri en yüksek olan epoch'un ağırlıkları saklandı.

| model | dosya | batch | en iyi epoch | eğitim süresi | boyut |
|---|---|---:|---:|---:|---:|
| YOLO26n | `exp-3.pt` | 275 | 83 | 58.3 dk | 5.5 MB |
| YOLO26l | `exp-4.pt` | 82 | 62 | 3.38 sa | 53.0 MB |

Test setindeki başarım (1847 görüntü, 4564 kutu; PyTorch FP32):

| model | precision | recall | mAP50 | mAP50-95 |
|---|---:|---:|---:|---:|
| YOLO26n | 0.815 | 0.721 | 0.773 | 0.497 |
| YOLO26l | 0.850 | 0.778 | 0.821 | 0.566 |

Sınıf bazında AP (test seti):

| model | sınıf | AP50 | AP50-95 |
|---|---|---:|---:|
| YOLO26n | military-vehicle | 0.901 | 0.631 |
| YOLO26n | person | 0.646 | 0.362 |
| YOLO26l | military-vehicle | 0.945 | 0.716 |
| YOLO26l | person | 0.697 | 0.416 |

- Büyük model (`l`) mAP50-95'i 0.497'den 0.566'ya çıkardı (+0.069). Karşılığında model yaklaşık 10 kat büyüdü.
- Zor sınıf `person`: havadan bakıldığında insanlar birkaç pikselden oluşuyor, sıklıkla kamuflajlı ve açık veri setlerinde etiketleri tutarsız. Gerçek personelin YOLO26n'de %13'ü, YOLO26l'de %14'ü hiç tespit edilmedi.
- İki sınıf birbiriyle neredeyse hiç karışmıyor; hatalar arka planla ilgili.
- YOLO26l 62. epoch'tan sonra hafif aşırı öğrenme gösterdi (doğrulama mAP50-95 0.563'ten 0.550'ye); en iyi epoch'un ağırlıkları saklandığı için sonucu etkilemedi.

### 4. Quantization: TensorRT FP16

Modeller, laptop GPU'sunda (6 GB) çalışacak şekilde TensorRT engine olarak export edildi:

- **Akış:** `.pt` (FP32) → ONNX (simplify) → TensorRT engine (FP16, batch=1, imgsz=640)
- **FP16:** Ağırlıklar ve aktivasyonlar 32 bit yerine 16 bit kayan noktalı sayı olarak tutuluyor. INT8'in aksine kalibrasyon verisi gerektirmiyor.
- `workspace=2` GB ile sınırlandı (6 GB VRAM).

Hızlanmanın ne kadarının yarı hassasiyetten, ne kadarının TensorRT'den geldiğini ayırmak için her model üç biçimde ölçüldü (test seti; NVIDIA RTX 3050 6 GB Laptop GPU, batch=1):

| model | biçim | boyut (MB) | çıkarım (ms) | hızlanma | uçtan uca FPS | mAP50 | mAP50-95 |
|---|---|---:|---:|---:|---:|---:|---:|
| YOLO26n | PyTorch FP32 | 5.5 | 10.1 | 1.0 kat | 64 | 0.773 | 0.497 |
| YOLO26n | PyTorch FP16 | 5.5 | 11.7 | 0.9 kat | 58 | 0.772 | 0.496 |
| YOLO26n | TensorRT FP16 | 62.0 | 2.6 | 4.0 kat | 123 | 0.772 | 0.496 |
| YOLO26l | PyTorch FP32 | 53.0 | 31.4 | 1.0 kat | 27 | 0.821 | 0.566 |
| YOLO26l | PyTorch FP16 | 53.0 | 19.2 | 1.6 kat | 41 | 0.822 | 0.566 |
| YOLO26l | TensorRT FP16 | 201.6 | 7.1 | 4.4 kat | 79 | 0.818 | 0.567 |

*(Çıkarım süresi yalnızca model içindir: boş GPU'da 500 görüntünün ortalaması, ilk 50 görüntü ısınma. Uçtan uca FPS görüntü okuma, ön ve son işlemeyi de içerir. PyTorch FP16 aynı `.pt` dosyasını kullanır; ağırlıklar yüklenirken FP16'ya çevrilir.)*

- **Doğruluk korunuyor:** mAP50-95 farkı iki modelde de 0.001'in altında (YOLO26n −0.0002, YOLO26l +0.001); sınıf bazındaki farklar ±0.4 puanı geçmiyor.
- **Hızlanma TensorRT'den geliyor:** PyTorch'ta FP16 büyük modeli yalnızca 1.6 kat hızlandırıyor, küçük modeli ise yavaşlatıyor. Aynı FP16 hassasiyetinde TensorRT, PyTorch FP16'ya göre YOLO26l'de 2.7, YOLO26n'de 4.6 kat daha hızlı. Yani kazancın büyük kısmı yarı hassasiyetten değil, TensorRT'nin grafik optimizasyonlarından (katman birleştirme, çekirdek seçimi) geliyor.
- **Engine dosyaları daha büyük:** `.engine` yalnızca ağırlıkları değil, hedef GPU için derlenmiş çekirdekleri de içeriyor. Ağırlıkların kendisi yarıya iniyor: ONNX dosyası YOLO26l'de 99.4 MB'tan 49.9 MB'a, YOLO26n'de 9.9 MB'tan 5.0 MB'a düştü.
- **Darboğaz modelin dışına kayıyor:** Görüntü başına ön işleme yaklaşık 1.2 ms, son işleme 1.5–1.8 ms, diskten okuma yaklaşık 2.5 ms sürüyor. Bu yüzden uçtan uca hızlanma (YOLO26n 1.9, YOLO26l 2.9 kat) model çıkarımındaki hızlanmadan düşük.

#### Ön çalışma: `exp-2` (raporda yok)

İlk quantization denemesi 4 sınıflı `exp-2` modeliyle (YOLO26n, AdamW, `dataset_clean`) yapılmıştı. `dataset_clean` test setinde (509 görüntü, 896 kutu) mAP50-95 0.586'dan 0.562'ye indi, çıkarım 13.6 ms'den 2.4 ms'ye düştü (5.8 kat). `models/exp-2.engine` bu denemenin çıktısıdır ve `detect.py`'nin varsayılan modelidir.

### 5. Canlı tespit (`detect.py`)

- Kamera, video dosyası ya da RTSP akışı (İHA video linki) kaynak olarak kullanılabilir.
- Webcam için MJPG formatı zorlanıyor. Aksi halde DirectShow 720p'de sıkıştırmasız YUY2 seçiyor ve FPS yaklaşık 10'a düşüyor.
- Ekranda FPS, kamera okuma süresi, model süresi ve sınıf başına tespit sayısı gösteriliyor.

### 6. Sınırlılıklar

- Hız ölçümleri bir laptop GPU'sunda yapıldı. Jetson gibi gömülü kartlarda engine'in o donanım için yeniden derlenmesi ve hızın orada ölçülmesi gerekir.
- INT8 quantization ve küçük nesneler için parçalı çıkarım (SAHI) denenmedi.
- `person` sınıfının başarımı düşük (AP50-95 0.36–0.42); etiketlerin iyileştirilmesi gerekiyor.
