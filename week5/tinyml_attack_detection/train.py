"""drop_dataset.csv ile kucuk bir MLP egitir ve ESP32 icin C header'a donusturur.

Model: 10 ozellik -> 16 -> 8 -> 1 (ReLU, sigmoid cikis)
Cikti:  detector/model.h  (normalizasyon + agirliklar, float32)

Kullanim:
    python train.py
"""
from pathlib import Path

import numpy as np
import pandas as pd
from sklearn.metrics import accuracy_score, confusion_matrix
from sklearn.model_selection import train_test_split
from sklearn.neural_network import MLPClassifier
from sklearn.preprocessing import StandardScaler

KLASOR = Path(__file__).resolve().parent   # tinyml_attack_detection/

# ======================= PARAMETRELER =======================
CSV = KLASOR / "drop_dataset.csv"
CIKTI = KLASOR / "detector" / "model.h"
EPOCHS = 400
# ============================================================

# ts zaman damgasi, avg_noise her satirda sabit (-96), label hedef -> ozellik degil.
# avg_rssi/min_rssi/avg_snr/min_snr mutlak seviyeler: kartlarin konumuna bagli,
# model bunlari ezberleyip kartlar yer degisince her seyi atak saniyordu -> cikarildi.
OZELLIKLER = [
    "received", "lost", "loss_ratio", "max_consec_lost",
    "avg_gap_ms", "gap_std_ms", "max_gap_ms",
    "rssi_std", "retry_ratio", "sniff_frames",
]


def c_dizi(ad, a):
    a = np.asarray(a, np.float32).ravel()
    return f"const float {ad}[{a.size}] = {{ {', '.join(f'{v:.8g}f' for v in a)} }};\n"


def header_yaz(yol, mean, std, katmanlar):
    """Modeli detector.ino'nun bekledigi C dizileri olarak yazar."""
    with open(yol, "w") as f:
        f.write("// train.py tarafindan uretildi, elle degistirme\n#pragma once\n\n")
        f.write(f"#define N_OZELLIK {len(OZELLIKLER)}\n")
        f.write("// Ozellik sirasi: " + ", ".join(OZELLIKLER) + "\n\n")
        f.write(c_dizi("OZ_MEAN", mean))
        f.write(c_dizi("OZ_STD", std))
        for i, (W, b) in enumerate(katmanlar, 1):
            f.write(f"\n#define L{i}_GIRIS {W.shape[1]}\n#define L{i}_CIKIS {W.shape[0]}\n")
            f.write(c_dizi(f"M_W{i}", W))   # satir = cikis noroni, [cikis][giris]
            f.write(c_dizi(f"M_B{i}", b))


def main():
    # Veriyi oku, bozuk satirlari at
    df = pd.read_csv(CSV, on_bad_lines="skip")
    df = df[OZELLIKLER + ["label"]].apply(pd.to_numeric, errors="coerce").dropna()
    df = df[df["label"].isin([0, 1])]
    X, y = df[OZELLIKLER].to_numpy(np.float32), df["label"].to_numpy(int)
    print(f"Veri: {len(y)} satir (normal={(y == 0).sum()}, atak={(y == 1).sum()})")

    # Sinif dengeli %80 egitim / %20 test
    X_tr, X_te, y_tr, y_te = train_test_split(X, y, test_size=0.2, stratify=y, random_state=0)

    # Normalizasyon: (x - mean) / std, sadece egitim verisinden hesaplanir
    olcek = StandardScaler().fit(X_tr)

    # MLP: 16 ve 8 noronlu iki gizli katman, ReLU; ikili siniflamada cikis sigmoid
    model = MLPClassifier(hidden_layer_sizes=(16, 8), activation="relu", alpha=1e-4,
                          learning_rate_init=1e-2, max_iter=EPOCHS, random_state=0)
    model.fit(olcek.transform(X_tr), y_tr)

    tahmin = model.predict(olcek.transform(X_te))
    tn, fp, fn, tp = confusion_matrix(y_te, tahmin).ravel()
    print(f"\nTest ({len(y_te)} satir): dogruluk = {accuracy_score(y_te, tahmin):.3f}")
    print(f"  Karisiklik matrisi   tahmin=0  tahmin=1")
    print(f"  gercek=0 (normal)    {tn:8d}  {fp:8d}")
    print(f"  gercek=1 (atak)      {fn:8d}  {tp:8d}")

    # sklearn agirliklari [giris][cikis] tutar, C kodu [cikis][giris] bekliyor -> transpoz
    katmanlar = [(W.T, b) for W, b in zip(model.coefs_, model.intercepts_)]

    # ESP'deki C kodunun aynisini numpy ile calistir, sklearn ile ayni sonucu vermeli
    h = olcek.transform(X_te)
    for i, (W, b) in enumerate(katmanlar):
        h = h @ W.T + b
        if i < len(katmanlar) - 1:
            h = np.maximum(h, 0)
    fark = np.abs(1 / (1 + np.exp(-h[:, 0])) - model.predict_proba(olcek.transform(X_te))[:, 1]).max()
    print(f"\nC-esdegeri ileri gecis kontrolu: max fark = {fark:.2e}")

    header_yaz(CIKTI, olcek.mean_, olcek.scale_, katmanlar)
    n_param = sum(W.size + b.size for W, b in katmanlar)
    print(f"Model yazildi: {CIKTI}  ({n_param} parametre, {n_param * 4} bayt)")


if __name__ == "__main__":
    main()
