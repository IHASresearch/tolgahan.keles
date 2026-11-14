# Midterm: İHA Görüntülerinden Askeri Araç Tespiti ve Model Quantization

İHA ya da yer kamerasından alınan görüntülerde **askeri araç** ve **personel** tespit eden bir YOLO modeli geliştirdik. Ardından modeli gömülü veya uçuş bilgisayarında gerçek zamanlı çalışabilmesi için **TensorRT FP16** formatına quantize ettik.

Projenin adımları:

1. Açık Roboflow datasetlerini indirmek, analiz etmek ve temizlemek
2. Birden fazla dataseti sızıntısız şekilde tek bir datasette birleştirmek
3. YOLO26'yı bu veriyle fine-tune etmek (Ultralytics Platform)
4. Modeli TensorRT FP16'ya quantize edip doğruluk ve hız kaybını ölçmek
5. Kamera veya video üzerinde canlı tespit yapmak

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

COCO üzerinde önceden eğitilmiş YOLO26 modelleri fine-tune edildi. Tüm modellerde `imgsz=640` ve 100 epoch kullanıldı.

| model | taban | veri | optimizer | precision | recall | mAP50 | mAP50-95 |
|---|---|---|---|---:|---:|---:|---:|
| exp-2 | yolo26n | dataset_clean (4 sınıf) | AdamW | 0.793 | 0.741 | 0.787 | 0.594 |
| exp-3 | yolo26n | dataset_merged (2 sınıf) | MuSGD | 0.807 | 0.702 | 0.753 | 0.494 |
| exp-4 | yolo26l | dataset_merged (2 sınıf) | MuSGD | 0.828 | 0.761 | 0.802 | 0.563 |

*(Değerler kendi validation setlerinde ölçüldü. exp-2 ile exp-3/4 farklı veri ve sınıf setleri kullandığı için doğrudan karşılaştırılamaz.)*

- exp-3, exp-2'ye göre daha zor bir görevde çalışıyor: havadan çekilmiş, küçük nesneler içeren ve çeşitli kaynaklardan gelen görüntüler. Bu yüzden mAP50-95 düşük görünüyor, ama İHA senaryosuna daha uygun.
- exp-4'te büyük model (`l`) aynı veride mAP50-95'i 0.494'ten 0.563'e çıkardı. Bunun karşılığında model boyutu yaklaşık 10 kat büyüdü (5.5 MB'dan 53 MB'a).

### 4. Quantization: TensorRT FP16

Model, laptop GPU'sunda (6 GB) çalışacak şekilde TensorRT engine olarak export edildi:

- **Akış:** `.pt` (FP32) → ONNX (simplify) → TensorRT engine (FP16, batch=1, imgsz=640)
- **FP16:** Ağırlıklar ve aktivasyonlar 32 bit yerine 16 bit kayan noktalı sayı olarak tutuluyor. INT8'in aksine kalibrasyon verisi gerektirmiyor.
- `workspace=2` GB ile sınırlandı (6 GB VRAM).

Sonuçlar (`exp-2`, `dataset_clean` test seti: 509 görüntü, 896 kutu; NVIDIA RTX 3050 6 GB Laptop GPU, batch=1):

| format | mAP50 | mAP50-95 | çıkarım süresi | ~FPS (sadece model) |
|---|---:|---:|---:|---:|
| PyTorch `.pt` | 0.778 | 0.586 | 13.6 ms | ~74 |
| TensorRT FP16 `.engine` | 0.755 | 0.562 | 2.4 ms | ~420 |
| **fark** | **−0.023** | **−0.024** | **5.8 kat hızlı** | |

Sınıf bazında mAP50-95 (`.pt` → `.engine`): armored-vehicle 0.745 → 0.735, person 0.301 → 0.283, tank 0.772 → 0.759, truck 0.525 → 0.469.

Sonuç olarak yaklaşık 2 puanlık mAP kaybına karşılık çıkarım 5.8 kat hızlandı. En büyük kayıp, test setinde en az örneği olan `truck` sınıfında (67 kutu) görüldü. Bu hızda model, kamera FPS'inin çok üzerinde çalışıyor ve gerçek zamanlı İHA uygulaması için yeterli.

### 5. Canlı tespit (`detect.py`)

- Kamera, video dosyası ya da RTSP akışı (İHA video linki) kaynak olarak kullanılabilir.
- Webcam için MJPG formatı zorlanıyor. Aksi halde DirectShow 720p'de sıkıştırmasız YUY2 seçiyor ve FPS yaklaşık 10'a düşüyor.
- Ekranda FPS, kamera okuma süresi, model süresi ve sınıf başına tespit sayısı gösteriliyor.
