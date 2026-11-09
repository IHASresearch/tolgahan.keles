"""Server (receiver) ESP32'nin CSV cikisini dosyaya kaydeder.

Etiket client'tan geldigi icin burada ek bir is yok: satirlari yazar,
server "# TAMAM" deyince (500 normal + 500 atak) kendiliginden kapanir.

Kullanim:
    python logger.py
"""
import time
from pathlib import Path

import serial

KLASOR = Path(__file__).resolve().parent   # tinyml_attack_detection/

# ======================= PARAMETRELER =======================
PORT = "COM5"     # server ESP32'nin portu
BAUD = 115200
CIKTI = KLASOR / time.strftime("drop_dataset_%Y%m%d_%H%M%S.csv")
# ============================================================


def main():
    sayac = {0: 0, 1: 0}
    baslik_var = False

    with serial.Serial(PORT, BAUD, timeout=1) as ser, open(CIKTI, "w", newline="") as f:
        time.sleep(2)          # port acilinca ESP32 resetlenebilir
        ser.write(b"h")        # basligi tekrar iste
        print(f"Kayit: {CIKTI}  (durdurmak icin Ctrl+C)")

        try:
            while True:
                satir = ser.readline().decode(errors="ignore").strip()
                if not satir:
                    continue
                if satir.startswith("#"):
                    print(satir)
                    if satir.startswith("# TAMAM"):
                        break
                elif satir.startswith("ts,"):
                    if not baslik_var:
                        f.write(satir + "\n")
                        baslik_var = True
                elif satir[0].isdigit() and baslik_var:
                    f.write(satir + "\n")
                    f.flush()
                    label = int(satir.rsplit(",", 1)[-1])
                    sayac[label] += 1
                    durum = "ATAK" if label == 1 else "norm"
                    print(f"[{durum}] {satir}   | normal={sayac[0]}/500 atak={sayac[1]}/500")
        except KeyboardInterrupt:
            pass

    print(f"\nBitti: normal={sayac[0]} atak={sayac[1]} -> {CIKTI}")


if __name__ == "__main__":
    main()
