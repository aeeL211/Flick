# Flick

Pengatur sensitivitas sentuh untuk game yang berjalan sebagai plugin **AxManager** tanpa root.
Flick mengambil alih touchscreen, mengalikan gerakan jari sesuai pengaturan, lalu mengirim ulang
lewat perangkat `uinput` virtual. Flick hanya aktif saat game target berada di foreground.

Dirancang untuk **Free Fire** dan **Free Fire Max**, tetapi daftar package bisa diubah lewat WebUI.

## Fitur

- Sensitivitas horizontal dan vertikal 1.0x sampai 5.0x, bisa disinkronkan.
- Zona sentuh: seluruh layar, setengah kiri, atau setengah kanan.
- Orientasi landscape 90 derajat dan 270 derajat.
- Kurva linear, akselerasi, atau deselerasi.
- Penstabil gerakan (One Euro filter) dengan kekuatan dan responsivitas yang bisa diatur.
- HW Touch Enhancer opsional (`debug.sf.hw`).
- Filter package: Flick hanya diterapkan saat aplikasi target di foreground.
- WebUI untuk mengatur semua opsi, melihat status, dan menghentikan daemon.
- Notifikasi saat status berubah antara aktif dan idle.

## Keamanan

- Jika proses daemon mati, kernel melepas grab dan sentuhan kembali normal.
- Flick tidak pernah mengambil alih touchscreen saat jari sedang menempel.
- Setelah `SYN_DROPPED`, jari virtual diangkat terlebih dahulu.
- Grab hanya dilepas saat tidak ada jari virtual yang menekan.
- Tidak ada pemanggilan `dumpsys`, `ps`, atau proses anak lain di jalur polling.

## Instalasi

1. Ambil `Flick-v*.zip` dari artifact GitHub Actions, atau build sendiri.
2. Di AxManager, buka **Plugins** lalu install zip tersebut.
3. Buka WebUI Flick.
4. Atur sensitivitas dan opsi lain, lalu tekan **Aktifkan**.
5. Status akan menjadi `active` saat package target di foreground, dan `idle` saat tidak.
6. Gunakan **Hentikan darurat** untuk mengembalikan sentuhan normal.

Plugin ini membutuhkan AxManager dengan server version minimal `19200` (`axeronPlugin=19200`
pada `module.prop`). Flick berjalan dengan hak akses shell dan tidak membutuhkan root.

## Build

Workflow GitHub Actions menjalankan selftest di host, membangun binary `arm64` dan `arm`,
memverifikasi ELF, lalu mengemas `Flick-v*.zip`.

Build lokal:

```bash
export NDK=$ANDROID_NDK_HOME
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
mkdir -p out/system/bin out/webroot

for t in aarch64-linux-android24:arm64 armv7a-linux-androideabi24:arm; do
  "$BIN/${t%%:*}-clang++" -Os -ffp-contract=off -DNDEBUG -Wall -Wextra -s \
    -nostdlib++ -fno-exceptions -fno-rtti -fvisibility=hidden \
    -fno-unwind-tables -fno-asynchronous-unwind-tables \
    -ffunction-sections -fdata-sections \
    -Wl,--gc-sections -Wl,--build-id=none \
    flick.cpp -o "out/system/bin/flick.${t##*:}" -lm
done

cp module.prop customize.sh uninstall.sh hw.sh ctl.sh notify.sh out/
cp index.html out/webroot/
cd out
zip -r "../Flick-$(grep '^version=' module.prop | cut -d= -f2).zip" .
```

`selftest.cpp` bisa dijalankan di host Linux:

```bash
g++ -std=c++17 -O2 -ffp-contract=off -Wall -Wextra \
  -fno-exceptions -fno-rtti selftest.cpp -o selftest-host
./selftest-host
```

## Konfigurasi

Semua key opsional. Nilai yang tidak valid diabaikan. WebUI menulis file `config` di folder
plugin dan daemon memuat ulang saat file berubah.

| Key | Default | Keterangan |
|---|---|---|
| `sens_h` | `1.0` | Sensitivitas sumbu horizontal, 1.0 sampai 5.0 |
| `sens_v` | `1.0` | Sensitivitas sumbu vertikal, 1.0 sampai 5.0 |
| `zone` | `0` | `0` seluruh layar, `1` kiri, `2` kanan |
| `orient` | `1` | `1` atau `3`, menentukan sisi kiri pada landscape |
| `smooth` | `0` | `1` mengaktifkan One Euro filter |
| `strength` | `40` | Kekuatan filter, 1 sampai 100 persen |
| `resp` | `85` | Responsivitas filter, 1 sampai 100 persen |
| `curve` | `0` | `0` linear, `1` akselerasi, `2` deselerasi |
| `hw` | `0` | `1` menjalankan `hw.sh 1` selama Flick aktif |
| `filter` | `1` | `1` hanya saat package target foreground, `0` selalu |
| `pkgs` | `com.dts.freefireth,com.dts.freefiremax` | Daftar package dipisah koma, maksimal 16 |

## Struktur file

| File | Fungsi |
|---|---|
| `flick.cpp` | Daemon, grab/uinput, kalkulasi sensitivitas, dan filter |
| `selftest.cpp` | Selftest kalkulasi yang bisa dijalankan di host |
| `index.html` | WebUI plugin |
| `ctl.sh` | Entry point WebUI: `status`, `save`, `start`, `stop` |
| `customize.sh` | Menyalin binary sesuai ABI saat instalasi |
| `uninstall.sh` | Menghentikan daemon dan mengembalikan properti |
| `hw.sh` | Mengubah dan mengembalikan `debug.sf.hw` |
| `notify.sh` | Notifikasi status aktif/idle |
| `module.prop` | Metadata plugin AxManager |

## Kredit

Kalkulasi sensitivitas (penggabungan sumbu X/Y, kurva, dan parameter filter) mengikuti fitur
**Magic Touch** di aplikasi **Game Corner** milik **FahrezONE**, yang diperoleh melalui reverse
engineering sebagaimana dicatat di `flick.cpp`. Bagian daemon, grab/uinput, deteksi foreground,
konfigurasi, dan WebUI merupakan bagian dari proyek ini.

## Catatan

- Efek dan kompatibilitas bergantung pada perangkat dan game.
- HW Touch Enhancer memakai `setprop debug.sf.hw`; jika tidak diizinkan, biarkan mati.
- Belum ada lisensi.
