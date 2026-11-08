"""
Tek bir YOLO datasetini (Roboflow -> YOLO export) analiz eder ve temizler.  [v1 - exp-2 bu veriyle egitildi]

    python src/data_cleaning.py analyze                  # sadece rapor
    python src/data_cleaning.py analyze --src C:\\datasets\\view6
    python src/data_cleaning.py clean                    # data/dataset_clean/ (4 sinif)
    python src/data_cleaning.py clean --zip              # + Ultralytics Platform icin zip

clean adimi: tum split'leri birlestirir, bozuk ve birebir tekrar goruntuleri atar,
sinif adlarini CLASS_MAP ile duzeltir, kaynak gorsele gore gruplu ve sinif dengeli
yeniden boler (SPLIT_RATIO), dosyalari kisa adlarla (train_00001.jpg) yazar ve
eski/yeni ad eslesmesini name_map.csv'ye kaydeder.
"""

import argparse
import shutil
from collections import Counter, defaultdict
from pathlib import Path

from common import (CLEAN_DIR, FATAL_ERRORS, IMG_EXT, RAW_MAIN, SPLITS, balanced_split, clamp_box,
                    md5, parse_line, read_names, source_id, write_data_yaml)

try:
    from PIL import Image
except ImportError:
    Image = None

SRC = RAW_MAIN
DST = CLEAN_DIR

# Eski sinif adi -> yeni sinif adi. None = o sinifin kutularini sil.
# Listede olmayan siniflar ayni isimle kalir.
CLASS_MAP: dict[str, str | None] = {
    "amoured-vehicle": "armored-vehicle",  # yazim hatasi
    "artillery": "armored-vehicle",        # sadece 49 kutu, tek basina ogrenilemez
}

MIN_BOX_SIDE = 0.0         # normalize kenar < bu deger olan kutular silinir (0 = kapali)
DROP_EMPTY_IMAGES = False  # etiketsiz (arka plan) goruntuleri at
SPLIT_RATIO = {"train": 0.80, "valid": 0.15, "test": 0.05}
SEED = 42


def image_size(p: Path):
    """(w, h) ya da bozuksa None. Pillow yoksa (0, 0)."""
    if Image is None:
        return (0, 0)
    try:
        with Image.open(p) as im:
            im.verify()
        with Image.open(p) as im:
            return im.size
    except Exception:
        return None


def iter_split(split: str):
    img_dir, lbl_dir = SRC / split / "images", SRC / split / "labels"
    if not img_dir.exists():
        return
    for img in sorted(img_dir.iterdir()):
        if img.suffix.lower() in IMG_EXT:
            yield img, lbl_dir / f"{img.stem}.txt"


# ---------------------------------------------------------------- analiz

SIZE_BINS = [(0.02, "<%2 (cok kucuk)"), (0.05, "%2-5 (kucuk)"), (0.15, "%5-15 (orta)"),
             (0.40, "%15-40 (buyuk)"), (9.0, ">%40 (kadraj dolu)")]


def size_bin(w, h):
    s = (w * h) ** 0.5  # goruntuye gore goreli kenar
    return next(label for lim, label in SIZE_BINS if s < lim)


def analyze():
    names = read_names(SRC / "data.yaml")
    n = len(names)
    print(f"Dataset: {SRC}\nSinif sayisi: {n}\n")

    inst = {s: Counter() for s in SPLITS}        # kutu sayisi
    img_per_cls = {s: Counter() for s in SPLITS}  # o sinifi iceren goruntu
    sizes = defaultdict(Counter)                  # sinif -> boyut kutusu
    errors = Counter()
    res = Counter()
    boxes_per_img = Counter()
    hashes = defaultdict(list)
    sources = defaultdict(set)
    summary = {}
    corrupt = []

    for split in SPLITS:
        imgs = missing = empty = 0
        for img, lbl in iter_split(split):
            imgs += 1
            hashes[md5(img)].append(f"{split}/{img.name}")
            sources[source_id(img)].add(split)
            size = image_size(img)
            if size is None:
                corrupt.append(f"{split}/{img.name}")
            elif Image:
                res[size] += 1
            if not lbl.exists():
                missing += 1
                continue
            lines = [l for l in lbl.read_text().splitlines() if l.strip()]
            if not lines:
                empty += 1
            boxes_per_img[min(len(lines), 10)] += 1
            seen = set()
            for line in lines:
                cls, cx, cy, w, h, err = parse_line(line, n)
                if err:
                    errors[err] += 1
                if cls is None or not 0 <= cls < n or w <= 0 or h <= 0:
                    continue
                inst[split][cls] += 1
                seen.add(cls)
                sizes[cls][size_bin(w, h)] += 1
            for c in seen:
                img_per_cls[split][c] += 1
        summary[split] = (imgs, missing, empty)

    print("=== Split ozeti ===")
    total = sum(v[0] for v in summary.values()) or 1
    for s, (i, m, e) in summary.items():
        print(f"{s:6s}: {i:6d} goruntu (%{100*i/total:4.1f}) | etiket dosyasi yok: {m} | bos etiket: {e}")

    print("\n=== Sinif dagilimi (kutu sayisi / goruntu sayisi) ===")
    print(f"{'id':>3} {'sinif':22s} " + " ".join(f"{s:>14s}" for s in SPLITS) + f" {'toplam':>8s}")
    for c in sorted(range(n), key=lambda c: -sum(inst[s][c] for s in SPLITS)):
        cols = " ".join(f"{inst[s][c]:6d}/{img_per_cls[s][c]:<7d}" for s in SPLITS)
        print(f"{c:3d} {names[c]:22s} {cols} {sum(inst[s][c] for s in SPLITS):8d}")

    print("\n=== Kutu boyutu (goreli kenar = sqrt(w*h)) ===")
    labels = [l for _, l in SIZE_BINS]
    print(f"{'sinif':22s} " + " ".join(f"{l.split()[0]:>8s}" for l in labels))
    for c in range(n):
        tot = sum(sizes[c].values()) or 1
        print(f"{names[c]:22s} " + " ".join(f"{100*sizes[c][l]/tot:7.1f}%" for l in labels))

    print("\n=== Goruntu basina kutu sayisi ===")
    for k in sorted(boxes_per_img):
        print(f"  {k if k < 10 else '10+':>3}: {boxes_per_img[k]}")

    print(f"\n=== Bozuk / acilamayan goruntu: {len(corrupt)} ===")
    for p in corrupt[:10]:
        print("   ", p)

    if res:
        print("\n=== Cozunurluk (tum split'ler) ===")
        for size, cnt in res.most_common(8):
            print(f"  {size[0]}x{size[1]}: {cnt}")
    else:
        print("\n(Pillow kurulu degil, cozunurluk analizi atlandi: pip install pillow)")

    print("\n=== Etiket hatalari ===")
    print("  yok" if not errors else "\n".join(f"  {k}: {v}" for k, v in errors.most_common()))

    dups = {h: v for h, v in hashes.items() if len(v) > 1}
    cross = [v for v in dups.values() if len({p.split('/')[0] for p in v}) > 1]
    print(f"\n=== Tekrar eden goruntuler (birebir ayni dosya) ===\n  grup: {len(dups)} | split'ler arasi: {len(cross)}")
    for v in cross[:5]:
        print("   ", v)

    leak = {k: v for k, v in sources.items() if len(v) > 1}
    print(f"\n=== Ayni kaynak gorselin (.rf. oncesi ad) farkli split'lerde olmasi ===\n  {len(leak)} kaynak")
    for k, v in list(leak.items())[:5]:
        print(f"    {k}: {sorted(v)}")


# ---------------------------------------------------------------- temizleme

def clean(make_zip: bool):
    old = read_names(SRC / "data.yaml")
    new_names = []
    for name in old:
        tgt = CLASS_MAP.get(name, name)
        if tgt is not None and tgt not in new_names:
            new_names.append(tgt)
    remap = {i: (new_names.index(CLASS_MAP.get(nm, nm)) if CLASS_MAP.get(nm, nm) is not None else None)
             for i, nm in enumerate(old)}

    # 1) tum split'leri oku, bozuk ve tekrar edenleri ele, etiketleri duzelt
    stats = Counter()
    seen_hash = set()
    groups = defaultdict(list)
    for split in SPLITS:
        for img, lbl in iter_split(split):
            if image_size(img) is None:
                stats["bozuk_goruntu_silindi"] += 1
                continue
            h = md5(img)
            if h in seen_hash:
                stats["tekrar_goruntu_silindi"] += 1
                continue
            seen_hash.add(h)

            out = []
            for line in (lbl.read_text().splitlines() if lbl.exists() else []):
                p = parse_line(line, len(old))
                if p is None:
                    continue
                cls, cx, cy, w, h_, err = p
                if err in FATAL_ERRORS:
                    stats[f"hatali_satir_{err}"] += 1
                    continue
                if remap[cls] is None:
                    stats["sinif_silindi_kutu"] += 1
                    continue
                cx, cy, w, h_ = clamp_box(cx, cy, w, h_)
                if min(w, h_) <= MIN_BOX_SIDE or w <= 0 or h_ <= 0:
                    stats["kucuk_kutu_silindi"] += 1
                    continue
                out.append(f"{remap[cls]} {cx:.6f} {cy:.6f} {w:.6f} {h_:.6f}")

            if not out and DROP_EMPTY_IMAGES:
                stats["bos_goruntu_silindi"] += 1
                continue
            groups[source_id(img)].append({"img": img, "split": split, "labels": out})

    # 2) gruplu + sinif dengeli yeniden bolme: her grup icerdigi en nadir sinifa gore katmanlanir
    class_total = Counter(int(l.split()[0]) for items in groups.values() for it in items for l in it["labels"])
    strata = defaultdict(list)
    for gid in sorted(groups):
        classes = {int(l.split()[0]) for it in groups[gid] for l in it["labels"]}
        strata[min(classes, key=lambda c: class_total[c]) if classes else -1].append(gid)
    split_of = balanced_split(strata, lambda g: len(groups[g]), SPLIT_RATIO, SEED)

    # 3) kisa adlarla yaz
    if DST.exists():
        shutil.rmtree(DST)
    for s in SPLITS:
        (DST / s / "images").mkdir(parents=True)
        (DST / s / "labels").mkdir(parents=True)
    idx = Counter()
    inst = {s: Counter() for s in SPLITS}
    rows = ["new_name,new_split,original_split,original_name"]
    for gid in sorted(groups):
        s = split_of[gid]
        for it in groups[gid]:
            idx[s] += 1
            stem = f"{s}_{idx[s]:05d}"
            ext = it["img"].suffix.lower()
            shutil.copy2(it["img"], DST / s / "images" / f"{stem}{ext}")
            (DST / s / "labels" / f"{stem}.txt").write_text(
                "\n".join(it["labels"]) + ("\n" if it["labels"] else ""))
            for l in it["labels"]:
                inst[s][int(l.split()[0])] += 1
            rows.append(f'{stem}{ext},{s},{it["split"]},"{it["img"].name}"')
    (DST / "name_map.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    write_data_yaml(DST, new_names)

    print(f"Yeni siniflar ({len(new_names)}): {new_names}")
    for k, v in sorted(stats.items()):
        print(f"  {k}: {v}")
    print("\n=== Yeni split'ler (goruntu | kutu sayisi sinif bazinda) ===")
    print(f"{'split':6s} {'goruntu':>8s} " + " ".join(f"{n[:14]:>15s}" for n in new_names))
    for s in SPLITS:
        print(f"{s:6s} {idx[s]:8d} " + " ".join(f"{inst[s][c]:15d}" for c in range(len(new_names))))
    print(f"\nCikti: {DST}")
    if make_zip:
        print("Zip:", shutil.make_archive(str(DST), "zip", DST))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=["analyze", "clean"])
    ap.add_argument("--src", type=Path, default=SRC, help="ham Roboflow dataset klasoru")
    ap.add_argument("--zip", action="store_true")
    a = ap.parse_args()
    SRC = a.src.resolve()
    analyze() if a.cmd == "analyze" else clean(a.zip)
