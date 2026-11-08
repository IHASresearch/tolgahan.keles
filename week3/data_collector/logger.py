"""Server ESP32'nin CSV cikisini kaydeder, ayni anda atak senaryosunu otomatik isletir.

Tek script uc isi birden yapar:
  1. Server'in seri portundan CSV satirlarini okur ve dosyaya yazar.
  2. Bir takvim isletir: NORMAL penceresi / ATAK penceresi, sonsuz dongu.
  3. ATAK penceresinde UAV_LINK agina hizli UDP trafigi basar (gercek kanal cakismasi).
     Etiketi kendi saatine gore yazdigi icin flood ile etiket her zaman senkron.

Flood'un ise yaramasi icin PC'nin UAV_LINK agina bagli olmasi gerekir
(server'in AP'si; IP ornegi 192.168.4.x). Ucuncu bir ESP'ye gerek yok.

Kullanim:
    python logger.py COM5                      # varsayilan 60 sn normal / 30 sn atak
    python logger.py COM5 -o dataset.csv --normal 90 --attack 30
    python logger.py COM5 --no-flood           # sadece etiketle, flood'u sen yap
    python logger.py COM5 --manual             # takvim yok, server'in kendi etiketini kullan
"""
import argparse
import os
import socket
import threading
import time

import serial

stop = threading.Event()


def phase_label(t0, normal, attack):
    """Gecen sureye gore o anki etiketi dondurur: 0 = normal, 1 = atak."""
    e = (time.monotonic() - t0) % (normal + attack)
    return 0 if e < normal else 1


def flood_worker(t0, normal, attack, host, port):
    """ATAK penceresinde hedef aga yogun UDP broadcast basar."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    buf = b"\xAB" * 1400
    while not stop.is_set():
        if phase_label(t0, normal, attack) == 1:
            try:
                for _ in range(100):
                    s.sendto(buf, (host, port))
            except OSError:
                time.sleep(0.1)   # ag henuz hazir degilse
        else:
            time.sleep(0.05)
    s.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port", help="server ESP32'nin portu (or. COM5)")
    ap.add_argument("-b", "--baud", type=int, default=115200)
    ap.add_argument("-o", "--out", default=time.strftime("dataset_%Y%m%d_%H%M%S.csv"))
    ap.add_argument("--normal", type=int, default=60, help="normal penceresi (sn)")
    ap.add_argument("--attack", type=int, default=30, help="atak penceresi (sn)")
    ap.add_argument("--flood-host", default="192.168.4.255", help="UAV_LINK broadcast adresi")
    ap.add_argument("--flood-port", type=int, default=4211)
    ap.add_argument("--no-flood", action="store_true", help="flood yapma, sadece etiketle")
    ap.add_argument("--manual", action="store_true", help="takvim yok; server'in etiketini kullan")
    args = ap.parse_args()

    baslik_var = os.path.exists(args.out) and os.path.getsize(args.out) > 0
    sayac = {0: 0, 1: 0}
    t0 = time.monotonic()

    if not args.manual and not args.no_flood:
        threading.Thread(
            target=flood_worker,
            args=(t0, args.normal, args.attack, args.flood_host, args.flood_port),
            daemon=True,
        ).start()
        print(f"Flood aktif -> {args.flood_host}:{args.flood_port} "
              f"({args.normal} sn normal / {args.attack} sn atak)")
        print("NOT: PC'nin UAV_LINK agina bagli oldugundan emin ol.")

    with serial.Serial(args.port, args.baud, timeout=1) as ser, open(args.out, "a", newline="") as f:
        time.sleep(2)   # port acilinca ESP32 resetlenebilir
        ser.write(b"h")
        print(f"Kayit: {args.out}  (durdurmak icin Ctrl+C)")

        try:
            while True:
                satir = ser.readline().decode(errors="ignore").strip()
                if not satir:
                    continue
                if satir.startswith("#"):
                    print(satir)
                elif satir.startswith("ts,"):
                    if not baslik_var:
                        f.write(satir + "\n")
                        f.flush()
                        baslik_var = True
                elif satir[0].isdigit() and baslik_var:
                    if not args.manual:
                        # Etiketi kendi takvimimizle degistir (son sutun)
                        label = phase_label(t0, args.normal, args.attack)
                        satir = satir.rsplit(",", 1)[0] + f",{label}"
                    else:
                        label = int(satir.rsplit(",", 1)[-1] or 0)
                    f.write(satir + "\n")
                    f.flush()
                    sayac[label] = sayac.get(label, 0) + 1
                    durum = "ATAK" if label == 1 else "norm"
                    print(f"[{durum}] {satir}   | normal={sayac[0]} atak={sayac[1]}")
        except KeyboardInterrupt:
            pass
        finally:
            stop.set()
            print(f"\nBitti: normal={sayac[0]} atak={sayac[1]} -> {args.out}")


if __name__ == "__main__":
    main()
