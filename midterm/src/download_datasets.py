"""
Ek Roboflow datasetlerini C:\\datasets\\<isim> altina indirir (uzun dosya adlari icin kisa yol).
Ana dataset (huyen-dinh/military-vehicle-rstvv v2) C:\\military-vehicle-dataset altinda olmali.

    $env:ROBOFLOW_API_KEY="..."            (cmd: set ROBOFLOW_API_KEY=...)
    python src/download_datasets.py
"""

import os

from roboflow import Roboflow

from common import RAW_EXTRA

# kisa ad: (workspace, project, version)
DATASETS = {
    "roman": ("romanpertdrone", "military-lyxbx", 2),
    "gzhet": ("test-fo7by", "military-gzhet", 1),
    "uce":   ("uce03211-gmail-com", "yolo-military", 10),   # indirildi ama birlestirmede kullanilmadi
    "view6": ("datasets-tqcu8", "military-view6", 2),
    "mamad": ("mamad-vira", "military-vehicle-m5bvw", 1),
}

if __name__ == "__main__":
    rf = Roboflow(api_key=os.environ["ROBOFLOW_API_KEY"])
    RAW_EXTRA.mkdir(exist_ok=True)
    for name, (ws, proj, ver) in DATASETS.items():
        dst = RAW_EXTRA / name
        if (dst / "data.yaml").exists():
            print(f"[atla] {name} zaten var")
            continue
        print(f"[indir] {name}: {ws}/{proj} v{ver}")
        rf.workspace(ws).project(proj).version(ver).download("yolo26", location=str(dst))
