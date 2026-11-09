"""
YOLO modelini bir video uzerinde calistirir.
Ayarlari asagidaki PARAMETRELER kismindan degistirip calistirin:

    python main.py

Tuslar:  q / ESC = cikis   space = duraklat
"""

from pathlib import Path

import cv2
from ultralytics import YOLO

ROOT = Path(__file__).resolve().parent   # uav_detection/

# ======================= PARAMETRELER =======================
MODEL = ROOT / "uav_detection_model.pt"
VIDEO = ROOT / "videos" / "video2.webm"
CONF = 0.25           # guven esigi
IMGSZ = 640           # model giris boyutu
SHOW = True           # pencerede goster
SAVE = False          # ciktiyi runs/run_video/ altina kaydet
# ============================================================


model = YOLO(MODEL)
cap = cv2.VideoCapture(str(VIDEO))
fps = cap.get(cv2.CAP_PROP_FPS) or 30   # kayit videosu kaynakla ayni hizda olsun

writer = None
if SAVE:
    out_dir = ROOT / "runs" / "run_video"
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / f"{VIDEO.stem}_{MODEL.stem}.mp4"   # or. video2_uav_detection_model.mp4

while True:
    ok, frame = cap.read()
    if not ok:          # video bitti
        break

    # Kareyi modele ver, tespit edilen kutulari kare uzerine ciz
    r = model.predict(frame, imgsz=IMGSZ, conf=CONF, device=0, verbose=False)[0]
    vis = r.plot(line_width=2)

    if SAVE:
        if writer is None:   # boyut ilk kareden belli oldugu icin writer burada acilir
            writer = cv2.VideoWriter(str(out_path), cv2.VideoWriter_fourcc(*"mp4v"),
                                     fps, (vis.shape[1], vis.shape[0]))
        writer.write(vis)

    if SHOW:
        cv2.imshow("IHA tespiti", vis)
        k = cv2.waitKey(1) & 0xFF
        if k in (ord("q"), 27):   # q veya ESC: cikis
            break
        if k == ord(" "):         # bosluk: herhangi bir tusa basilana kadar duraklat
            cv2.waitKey(0)

cap.release()
if writer:
    writer.release()
    print("Kayit:", out_path)
cv2.destroyAllWindows()
