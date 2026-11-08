"""
Egitilmis modeli bu PC'nin GPU'su icin TensorRT FP16'ya export eder (quantization) ve
.pt ile .engine'i ayni test setinde karsilastirir.

    python src/export_model.py                              # models/exp-2.pt
    python src/export_model.py --weights models/exp-4.pt
    python src/export_model.py --weights models/exp-4.pt --skip-export   # sadece karsilastir

Test seti modelin sinif sayisina gore secilir:
    4 sinif (exp-2)        -> data/dataset_clean
    2 sinif (exp-3, exp-4) -> data/dataset_merged  (+ sadece havadan cekilmis goruntuler: test_aerial)
"""

import argparse
from pathlib import Path

import torch
from ultralytics import YOLO

from common import CLEAN_DIR, MERGED_DIR, MODELS_DIR, RUNS_DIR

AERIAL_PREFIX = ("view6_", "mamad_")  # havadan cekilmis datasetler


def aerial_yaml() -> Path:
    """dataset_merged test setinin sadece havadan cekilmis kismi icin ayri bir data yaml.
    Yollar goreli yazilir; 'path' verilmediginde Ultralytics yaml'in klasorunu kok kabul eder."""
    imgs = sorted(p for p in (MERGED_DIR / "test" / "images").iterdir() if p.name.startswith(AERIAL_PREFIX))
    (MERGED_DIR / "test_aerial.txt").write_text(
        "".join(f"./test/images/{p.name}\n" for p in imgs), encoding="utf-8")
    y = MERGED_DIR / "data_aerial.yaml"
    y.write_text("train: test_aerial.txt\nval: test_aerial.txt\ntest: test_aerial.txt\n\n"
                 "nc: 2\nnames:\n  0: military-vehicle\n  1: person\n", encoding="utf-8")
    print(f"Havadan test goruntusu: {len(imgs)}")
    return y


def test_sets(n_cls: int) -> dict:
    if n_cls == 4:
        return {"test": CLEAN_DIR / "data.yaml"}
    if n_cls == 2:
        return {"test": MERGED_DIR / "data.yaml", "test_aerial": aerial_yaml()}
    raise SystemExit(f"Beklenmeyen sinif sayisi: {n_cls}")


def evaluate(weights: Path, data: Path, tag: str, imgsz: int):
    m = YOLO(str(weights), task="detect")
    r = m.val(data=str(data), split="test", imgsz=imgsz, batch=1, device=0, half=True,
              plots=False, verbose=False, project=str(RUNS_DIR / "export_val"), name=tag, exist_ok=True)
    per_cls = {m.names[c]: round(float(r.box.maps[c]), 3) for c in range(len(m.names))}
    return {"mAP50": round(float(r.box.map50), 3), "mAP50-95": round(float(r.box.map), 3),
            "per_class_mAP50-95": per_cls, "ms/img": round(r.speed["inference"], 2)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--weights", default=str(MODELS_DIR / "exp-2.pt"))
    ap.add_argument("--imgsz", type=int, default=640)
    ap.add_argument("--skip-export", action="store_true", help="var olan .engine dosyasini kullan")
    a = ap.parse_args()
    pt = Path(a.weights).resolve()

    assert torch.cuda.is_available(), "CUDA'li torch gerekli (pip install torch --index-url .../cu128)"
    print("GPU:", torch.cuda.get_device_name(0))

    model = YOLO(str(pt))
    print("Siniflar:", model.names, "| egitim imgsz:", model.ckpt.get("train_args", {}).get("imgsz"))

    if a.skip_export:
        engine = pt.with_suffix(".engine")
        assert engine.exists(), f"{engine} yok, --skip-export olmadan calistirin"
    else:
        # 6 GB laptop GPU icin workspace sinirli tutuldu
        engine = Path(model.export(format="engine", half=True, imgsz=a.imgsz, batch=1,
                                   device=0, workspace=2, simplify=True))
    print("Engine:", engine)

    sets = test_sets(len(model.names))
    results = {}
    for w in (pt, engine):
        for name, data in sets.items():
            results[(w.suffix, name)] = evaluate(w, data, f"{w.stem}{w.suffix.replace('.', '_')}_{name}", a.imgsz)

    print("\n================ KARSILASTIRMA ================")
    for (fmt, name), r in results.items():
        print(f"{fmt:8s} {name:12s} mAP50={r['mAP50']:.3f}  mAP50-95={r['mAP50-95']:.3f}  "
              f"{r['ms/img']:.2f} ms/img  {r['per_class_mAP50-95']}")
    for name in sets:
        d = results[(".engine", name)]["mAP50"] - results[(".pt", name)]["mAP50"]
        s = results[(".pt", name)]["ms/img"] / max(results[(".engine", name)]["ms/img"], 1e-6)
        print(f"FP16 ({name}): mAP50 farki {d:+.3f}, hizlanma x{s:.1f}")


if __name__ == "__main__":
    main()
