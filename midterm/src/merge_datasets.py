"""
Birden fazla Roboflow YOLO datasetini tek, temiz bir datasette birlestirir.

    python src/merge_datasets.py            # data/dataset_merged/ olusturur
    python src/merge_datasets.py --zip      # + Ultralytics Platform icin zip

Once download_datasets.py ile ek datasetler indirilmis olmali.  [v2 - exp-3 / exp-4 bu veriyle egitildi]

Adimlar:
  1. Her dataset icin sinif eslestirme (SOURCES) -> military-vehicle / person
  2. Roboflow augment kopyalari ('isim_jpg.rf.<hash>') ayni kaynak adini tasir: hepsi ayni
     gruba (= ayni split'e) alinir, boylece train -> valid/test sizintisi olmaz
  3. Birebir ayni dosyalar (md5) ve neredeyse ayni goruntuler (dHash) bulunur:
       - hamming <= DUP_EXACT  -> tekrar sayilir, tek kopya kalir (oncelik: SOURCES sirasi)
       - hamming <= DUP_NEAR   -> silinmez ama ayni gruba alinir (ornegin ardisik video kareleri)
  4. Ayni videodan gelen kareler (dosya adindan) ayni gruba alinir
  5. Gruplar dataset bazinda katmanli olarak train/valid/test'e bolunur
     -> her dataset (ozellikle havadan cekilmis view6, mamad) her split'te oranli bulunur
  6. Dosyalar '<dataset>_<no>.jpg' seklinde kisa adlarla yazilir; name_map.csv eski adlari tutar
"""

import argparse
import re
import shutil
from collections import Counter, defaultdict
from concurrent.futures import ProcessPoolExecutor
from pathlib import Path

from PIL import Image

from common import (FATAL_ERRORS, IMG_EXT, MERGED_DIR, RAW_EXTRA, RAW_MAIN, SPLITS, balanced_split,
                    clamp_box, md5, parse_line, read_names, source_id, write_data_yaml)

DST = MERGED_DIR
NAMES = ["military-vehicle", "person"]
MV, PER = 0, 1

# ad: (klasor, {eski sinif: yeni sinif id | None})   -- sira = tekrar durumunda oncelik
SOURCES = {
    "huyen": (RAW_MAIN, {
        "amoured-vehicle": MV, "artillery": MV, "tank": MV, "truck": MV, "person": PER}),
    "gzhet": (RAW_EXTRA / "gzhet", {
        "Armored_car": MV, "Person": PER, "Car": None}),
    "roman": (RAW_EXTRA / "roman", {
        "IFV": MV, "armored car": MV, "military truck": MV, "tank": MV, "soldier": PER,
        "civilian": None, "airplane": None, "fighter jet": None, "helicopter": None, "fpv": None}),
    "view6": (RAW_EXTRA / "view6", {"military vehicle": MV}),
    "mamad": (RAW_EXTRA / "mamad", {"military-vehicle": MV}),
}

SPLIT_RATIO = {"train": 0.80, "valid": 0.15, "test": 0.05}
SEED = 42
DUP_EXACT = 2   # dHash hamming esigi: tekrar
DUP_NEAR = 6    # dHash hamming esigi: ayni gruba al


# ---------------------------------------------------------------- yardimcilar

def dhash(path: str):
    """64-bit fark hash'i. En-boy orani yok sayilir, boylece 'stretch' ile yeniden
    boyutlanmis kopyalar da eslesir. Bozuk dosyada None."""
    try:
        with Image.open(path) as im:
            g = im.convert("L").resize((9, 8), Image.BILINEAR)
    except Exception:
        return path, None, None
    px = list(g.getdata())
    bits = 0
    for r in range(8):
        for c in range(8):
            bits = (bits << 1) | (px[r * 9 + c] > px[r * 9 + c + 1])
    return path, bits, md5(Path(path))


VIDEO_RULES = [
    (re.compile(r"^(.*?_(?:mp4|MP4|mov|MOV|avi|webm))[-_]\d+.*$"), r"\1"),   # klip_mp4-15
    (re.compile(r"^(bandicam-\d{4}-\d\d-\d\d-\d\d-\d\d)-.*$"), r"\1"),          # ayni dakika
    (re.compile(r"^(ezgif)-frame-\d+.*$"), r"\1"),
    (re.compile(r"^(KakaoTalk_\d+_\d+?)(?:_\d+)?_jpg$"), r"\1"),
]


def video_key(src: str) -> str:
    for pat, rep in VIDEO_RULES:
        if pat.match(src):
            return pat.sub(rep, src)
    return src


class DSU:
    def __init__(self, n):
        self.p = list(range(n))

    def find(self, x):
        while self.p[x] != x:
            self.p[x] = self.p[self.p[x]]
            x = self.p[x]
        return x

    def union(self, a, b):
        self.p[self.find(a)] = self.find(b)


# ---------------------------------------------------------------- ana akis

def collect():
    items, stats = [], Counter()
    for ds, (root, cmap) in SOURCES.items():
        names = read_names(root / "data.yaml")
        unknown = set(names) - set(cmap)
        assert not unknown, f"{ds}: eslenmemis sinif {unknown}"
        remap = {i: cmap[n] for i, n in enumerate(names)}

        # Roboflow augment kopyalari ayni kaynak adini tasir ('isim_jpg.rf.<hash>'). Ayni ad bazen
        # farkli goruntulere de verilmis (mamad: 3 kopya beklenirken 16'lik gruplar var), bu yuzden
        # kopyalar silinmez; ayni addaki her sey ayni gruba (= ayni split'e) alinir.
        chosen = []
        for split in SPLITS:
            img_dir = root / split / "images"
            if not img_dir.exists():
                continue
            for img in sorted(img_dir.iterdir()):
                if img.suffix.lower() in IMG_EXT:
                    chosen.append((source_id(img), split, img))

        for sid, split, img in chosen:
            lbl = root / split / "labels" / f"{img.stem}.txt"
            out = []
            for line in (lbl.read_text().splitlines() if lbl.exists() else []):
                p = parse_line(line, len(names))
                if p is None:
                    continue
                cls, cx, cy, w, h, err = p
                if err in FATAL_ERRORS:
                    stats[f"{ds}: hatali satir"] += 1
                    continue
                if remap[cls] is None:
                    stats[f"{ds}: silinen kutu ({names[cls]})"] += 1
                    continue
                cx, cy, w, h = clamp_box(cx, cy, w, h)
                if w > 0 and h > 0:
                    out.append(f"{remap[cls]} {cx:.6f} {cy:.6f} {w:.6f} {h:.6f}")
            items.append({"ds": ds, "img": img, "split": split, "labels": out,
                          "video": f"{ds}:{video_key(sid)}"})
    return items, stats


def main(make_zip: bool):
    items, stats = collect()
    print(f"Toplanan goruntu: {len(items)}")

    # --- hash'ler (paralel)
    with ProcessPoolExecutor() as ex:
        res = {p: (h, m) for p, h, m in ex.map(dhash, [str(it["img"]) for it in items], chunksize=64)}
    keep = []
    for it in items:
        h, m = res[str(it["img"])]
        if h is None:
            stats[f"{it['ds']}: bozuk goruntu"] += 1
            continue
        it["dhash"], it["md5"] = h, m
        keep.append(it)
    items = keep

    # --- benzer goruntu adaylari: 8 x 8-bit parca; hamming <= 7 ise en az bir parca ayni (guvercin yuvasi)
    n = len(items)
    dsu = DSU(n)
    dup_of = {}
    buckets = defaultdict(list)
    for i, it in enumerate(items):
        for k in range(8):
            buckets[(k, (it["dhash"] >> (8 * k)) & 0xFF)].append(i)
    order = {ds: r for r, ds in enumerate(SOURCES)}
    for idxs in buckets.values():
        if len(idxs) > 400:   # tek renkli / bos goruntuler; buyuk kova -> atla
            continue
        for a_i, a in enumerate(idxs):
            for b in idxs[a_i + 1:]:
                d = bin(items[a]["dhash"] ^ items[b]["dhash"]).count("1")
                same_file = items[a]["md5"] == items[b]["md5"]
                if same_file or d <= DUP_NEAR:
                    dsu.union(a, b)
                if same_file or d <= DUP_EXACT:
                    # dusuk oncelikli olani tekrar olarak isaretle
                    lo, hi = sorted((a, b), key=lambda x: (order[items[x]["ds"]], str(items[x]["img"])))
                    dup_of.setdefault(hi, lo)

    # ayni video -> ayni grup
    first_of_video = {}
    for i, it in enumerate(items):
        j = first_of_video.setdefault(it["video"], i)
        if j != i:
            dsu.union(i, j)

    for d, s in dup_of.items():
        a, b = items[d]["ds"], items[s]["ds"]
        stats[f"{a}: tekrar silindi" + ("" if a == b else f" ({b} ile ayni)")] += 1
    alive = [i for i in range(n) if i not in dup_of]

    # --- gruplari split'lere dagit (dataset bazinda katmanli)
    groups = defaultdict(list)
    for i in alive:
        groups[dsu.find(i)].append(i)
    strata = defaultdict(list)
    for g, mem in groups.items():
        main_ds = Counter(items[i]["ds"] for i in mem).most_common(1)[0][0]
        strata[main_ds].append(g)
    for ds in strata:
        strata[ds].sort(key=lambda g: str(items[groups[g][0]]["img"]))
    split_of = balanced_split(strata, lambda g: len(groups[g]), SPLIT_RATIO, SEED)

    # --- yaz
    if DST.exists():
        shutil.rmtree(DST)
    for s in SPLITS:
        (DST / s / "images").mkdir(parents=True)
        (DST / s / "labels").mkdir(parents=True)
    counter = Counter()
    table = defaultdict(Counter)  # (split, ds) -> goruntu/kutu
    rows = ["new_name,split,dataset,original_split,original_name"]
    for i in sorted(alive, key=lambda i: (items[i]["ds"], str(items[i]["img"]))):
        it = items[i]
        s = split_of[dsu.find(i)]
        counter[it["ds"]] += 1
        stem = f"{it['ds']}_{counter[it['ds']]:05d}"
        ext = it["img"].suffix.lower()
        shutil.copy2(it["img"], DST / s / "images" / f"{stem}{ext}")
        (DST / s / "labels" / f"{stem}.txt").write_text(
            "\n".join(it["labels"]) + ("\n" if it["labels"] else ""))
        table[(s, it["ds"])]["img"] += 1
        for l in it["labels"]:
            table[(s, it["ds"])][int(l.split()[0])] += 1
        rows.append(f'{stem}{ext},{s},{it["ds"]},{it["split"]},"{it["img"].name}"')
    (DST / "name_map.csv").write_text("\n".join(rows) + "\n", encoding="utf-8")
    dup_rows = ["removed,kept,hamming"] + [
        f'"{items[d]["img"]}","{items[s]["img"]}",{bin(items[d]["dhash"] ^ items[s]["dhash"]).count("1")}'
        for d, s in dup_of.items()]
    (DST / "duplicates.csv").write_text("\n".join(dup_rows) + "\n", encoding="utf-8")
    write_data_yaml(DST, NAMES)

    # --- rapor
    print("\n=== Islem ozeti ===")
    for k, v in sorted(stats.items()):
        print(f"  {k}: {v}")
    print("\n=== Split x dataset (goruntu / military-vehicle / person) ===")
    print(f"{'':8s}" + "".join(f"{ds:>22s}" for ds in SOURCES) + f"{'TOPLAM':>22s}")
    for s in SPLITS:
        cells, tot = [], Counter()
        for ds in SOURCES:
            c = table[(s, ds)]
            tot.update(c)
            cells.append(f"{c['img']:>7d}/{c[MV]:>6d}/{c[PER]:>6d}")
        print(f"{s:8s}" + "".join(f"{x:>22s}" for x in cells) + f"{tot['img']:>9d}/{tot[MV]:>6d}/{tot[PER]:>5d}")
    print(f"\nCikti: {DST}")
    if make_zip:
        print("Zip olusturuluyor...")
        print("Zip:", shutil.make_archive(str(DST), "zip", DST))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--zip", action="store_true")
    main(ap.parse_args().zip)
