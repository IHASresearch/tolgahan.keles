"""
Projenin ortak yollari ve YOLO etiket yardimcilari.
Diger scriptler buradan import eder; tek basina calistirilmaz.
"""

import ast
import hashlib
import re
from pathlib import Path

# ---------------------------------------------------------------- yollar

ROOT = Path(__file__).resolve().parent.parent   # midterm/
DATA_DIR = ROOT / "data"
MODELS_DIR = ROOT / "models"
VIDEOS_DIR = ROOT / "videos"
RUNS_DIR = ROOT / "runs"

CLEAN_DIR = DATA_DIR / "dataset_clean"     # v1: tek dataset, 4 sinif
MERGED_DIR = DATA_DIR / "dataset_merged"   # v2: 5 dataset, 2 sinif

# Ham Roboflow datasetleri: dosya adlari cok uzun oldugu icin (Windows MAX_PATH) kisa yollarda tutuluyor
RAW_MAIN = Path(r"C:\military-vehicle-dataset")   # huyen-dinh/military-vehicle-rstvv v2
RAW_EXTRA = Path(r"C:\datasets")                  # download_datasets.py buraya indirir

SPLITS = ["train", "valid", "test"]
IMG_EXT = {".jpg", ".jpeg", ".png", ".bmp", ".webp"}


# ---------------------------------------------------------------- yardimcilar

def read_names(yaml_path: Path) -> list[str]:
    """data.yaml icindeki sinif adlari (liste ya da dict formati)."""
    text = yaml_path.read_text(encoding="utf-8")
    try:
        import yaml
        names = yaml.safe_load(text)["names"]
        return list(names.values()) if isinstance(names, dict) else list(names)
    except ImportError:
        m = re.search(r"^names:\s*(\[.*?\])", text, re.M | re.S)
        if m:
            return list(ast.literal_eval(m.group(1)))
        # blok liste:  names:\n  - a\n  - b   veya  0: a
        block = re.search(r"^names:\s*\n((?:\s+.+\n?)+)", text, re.M)
        return [re.sub(r"^\s*(-|\d+:)\s*", "", l).strip().strip("'\"")
                for l in block.group(1).splitlines() if l.strip()]


def write_data_yaml(dst: Path, names: list[str]):
    (dst / "data.yaml").write_text(
        "train: train/images\nval: valid/images\ntest: test/images\n\n"
        f"nc: {len(names)}\nnames:\n" + "".join(f"  {i}: {n}\n" for i, n in enumerate(names)),
        encoding="utf-8")


def source_id(img: Path) -> str:
    """Roboflow augment kopyalari 'isim_jpg.rf.<hash>.jpg' seklinde; kaynak gorsel = '.rf.' oncesi."""
    return img.stem.split(".rf.")[0]


def md5(p: Path) -> str:
    return hashlib.md5(p.read_bytes()).hexdigest()


def parse_line(line: str, n_cls: int):
    """(cls, cx, cy, w, h, hata) dondurur. Poligon satirlari bbox'a cevrilir."""
    parts = line.split()
    if not parts:
        return None
    try:
        cls = int(float(parts[0]))
        vals = [float(v) for v in parts[1:]]
    except ValueError:
        return (None, 0, 0, 0, 0, "sayi_degil")
    err = None
    if len(vals) == 4:
        cx, cy, w, h = vals
    elif len(vals) >= 6 and len(vals) % 2 == 0:
        xs, ys = vals[0::2], vals[1::2]
        x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
        cx, cy, w, h = (x0 + x1) / 2, (y0 + y1) / 2, x1 - x0, y1 - y0
        err = "poligon"
    else:
        return (cls, 0, 0, 0, 0, "kolon_sayisi")
    if not 0 <= cls < n_cls:
        return (cls, cx, cy, w, h, "sinif_id_disinda")
    if w <= 0 or h <= 0:
        return (cls, cx, cy, w, h, "sifir_boyut")
    if min(cx - w / 2, cy - h / 2) < -1e-3 or max(cx + w / 2, cy + h / 2) > 1 + 1e-3:
        err = err or "sinir_disi"
    return (cls, cx, cy, w, h, err)


FATAL_ERRORS = ("sayi_degil", "kolon_sayisi", "sinif_id_disinda", "sifir_boyut")


def clamp_box(cx, cy, w, h):
    x0, y0 = max(0.0, cx - w / 2), max(0.0, cy - h / 2)
    x1, y1 = min(1.0, cx + w / 2), min(1.0, cy + h / 2)
    return (x0 + x1) / 2, (y0 + y1) / 2, x1 - x0, y1 - y0


def balanced_split(strata: dict, size_of, ratio: dict, seed: int) -> dict:
    """Her katmandaki gruplari split'lere dagitir; her katman her split'te ~ratio oraninda olur.
    strata: {katman: [grup, ...]} (gruplar sirali verilmeli; tekrarlanabilirlik icin),
    size_of(grup) -> goruntu sayisi. Donus: {grup: split}"""
    import random
    rng = random.Random(seed)
    out = {}
    for key in sorted(strata):
        gs = list(strata[key])
        rng.shuffle(gs)
        done = {s: 0 for s in ratio}
        for g in gs:
            size = size_of(g)
            total = sum(done.values()) + size
            s = max(ratio, key=lambda s: ratio[s] * total - done[s])
            done[s] += size
            out[g] = s
    return out
