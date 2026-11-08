"""
Kamera / video uzerinde canli askeri arac tespiti.

    python src/detect.py                                    # varsayilan kamera (0)
    python src/detect.py --source 1                         # ikinci kamera
    python src/detect.py --source videos/test_1.mp4 --save
    python src/detect.py --weights models/exp-4.pt

Tuslar:  q / ESC = cikis   s = ekran goruntusu   +/- = guven esigi
"""

import argparse
import time

import cv2
from ultralytics import YOLO

from common import MODELS_DIR, RUNS_DIR

DEFAULT_WEIGHTS = next((p for p in (MODELS_DIR / "exp-2.engine", MODELS_DIR / "exp-2.pt") if p.exists()), None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--weights", default=str(DEFAULT_WEIGHTS))
    ap.add_argument("--source", default="0", help="kamera no, video yolu veya rtsp://...")
    ap.add_argument("--conf", type=float, default=0.35)
    ap.add_argument("--imgsz", type=int, default=640)  # .engine 640 ile export edildi
    ap.add_argument("--width", type=int, default=1280)
    ap.add_argument("--height", type=int, default=720)
    ap.add_argument("--fps", type=int, default=30)
    ap.add_argument("--save", action="store_true", help="ciktiyi runs/camera/ altina mp4 kaydet")
    a = ap.parse_args()

    model = YOLO(a.weights, task="detect")
    print("Model:", a.weights, "| siniflar:", model.names)

    src = int(a.source) if a.source.isdigit() else a.source
    cap = cv2.VideoCapture(src, cv2.CAP_DSHOW) if isinstance(src, int) else cv2.VideoCapture(src)
    if isinstance(src, int):
        # MJPG once ayarlanmali: aksi halde DSHOW 720p'de sikistirmasiz YUY2 secer ve ~10 FPS'e takilir
        cap.set(cv2.CAP_PROP_FOURCC, cv2.VideoWriter_fourcc(*"MJPG"))
        cap.set(cv2.CAP_PROP_FRAME_WIDTH, a.width)
        cap.set(cv2.CAP_PROP_FRAME_HEIGHT, a.height)
        cap.set(cv2.CAP_PROP_FPS, a.fps)
    if not cap.isOpened():
        raise SystemExit(f"Kaynak acilamadi: {a.source}")
    fcc = int(cap.get(cv2.CAP_PROP_FOURCC))
    print(f"Kamera: {int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))}x{int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))} "
          f"@ {cap.get(cv2.CAP_PROP_FPS):.0f} FPS, format {fcc.to_bytes(4, 'little').decode(errors='replace')}")

    writer = None
    out_dir = RUNS_DIR / "camera"
    out_dir.mkdir(parents=True, exist_ok=True)

    conf, fps, t_prev = a.conf, 0.0, time.perf_counter()
    win = "Askeri arac tespiti  (q: cikis)"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)

    read_ms = 0.0
    while True:
        t0 = time.perf_counter()
        ok, frame = cap.read()
        read_ms = 0.9 * read_ms + 0.1 * (time.perf_counter() - t0) * 1000
        if not ok:
            break

        r = model.predict(frame, imgsz=a.imgsz, conf=conf, device=0, verbose=False)[0]
        vis = r.plot(line_width=2)

        now = time.perf_counter()
        fps = 0.9 * fps + 0.1 * (1.0 / max(now - t_prev, 1e-6))
        t_prev = now
        counts = {}
        for c in r.boxes.cls.int().tolist():
            counts[model.names[c]] = counts.get(model.names[c], 0) + 1
        info = f"FPS {fps:5.1f} | kamera {read_ms:.0f} ms | model {r.speed['inference']:.1f} ms | conf {conf:.2f} | " + \
               (", ".join(f"{k}: {v}" for k, v in counts.items()) or "tespit yok")
        cv2.rectangle(vis, (0, 0), (vis.shape[1], 30), (0, 0, 0), -1)
        cv2.putText(vis, info, (8, 21), cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 1, cv2.LINE_AA)

        if a.save:
            if writer is None:
                path = out_dir / f"out_{time.strftime('%Y%m%d_%H%M%S')}.mp4"
                writer = cv2.VideoWriter(str(path), cv2.VideoWriter_fourcc(*"mp4v"), 30, (vis.shape[1], vis.shape[0]))
                print("Kayit:", path)
            writer.write(vis)

        cv2.imshow(win, vis)
        k = cv2.waitKey(1) & 0xFF
        if k in (ord("q"), 27):
            break
        if k == ord("s"):
            p = out_dir / f"shot_{time.strftime('%Y%m%d_%H%M%S')}.jpg"
            cv2.imwrite(str(p), vis)
            print("Kaydedildi:", p)
        if k in (ord("+"), ord("=")):
            conf = min(conf + 0.05, 0.95)
        if k == ord("-"):
            conf = max(conf - 0.05, 0.05)

    cap.release()
    if writer:
        writer.release()
    cv2.destroyAllWindows()


if __name__ == "__main__":
    main()
