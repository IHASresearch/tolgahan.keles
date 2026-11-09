# İHA Tespiti (YOLO26n)

video2 ile fine-tune edilmiş YOLO26n modeliyle videolarda İHA tespiti.

## Dosyalar

```
uav_detection/
├── main.py                  # modeli bir video üzerinde çalıştırır
├── uav_detection_model.pt   # video2 ile fine-tune edilmiş YOLO26n (1 sınıf: IHA)
└── videos/
    ├── video1.mp4           # test videosu
    └── video2.webm          # eğitim videosu
```

## Kullanım

`main.py` dosyasının üstündeki **PARAMETRELER** kısmını düzenleyip çalıştırın:

```python
MODEL = ROOT / "uav_detection_model.pt"
VIDEO = ROOT / "videos" / "video2.webm"   # test için video1.mp4
CONF = 0.25     # güven eşiği
IMGSZ = 640     # model giriş boyutu
SHOW = True     # pencerede göster
SAVE = False    # çıktıyı runs/run_video/ altına kaydet
```

```powershell
python main.py
```

Tuşlar: `q` / `ESC` çıkış, `boşluk` duraklat.
