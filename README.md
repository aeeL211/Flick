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
- Flick tidak mempertahankan grab saat jari sedang menempel. Pemeriksaan dilakukan sebelum dan
  sesudah `EVIOCGRAB`; jika jari mendarat di celah di antara keduanya, grab langsung dilepas dan
  dicoba lagi.
- Setelah `SYN_DROPPED`, jari virtual diangkat terlebih dahulu.
- Grab hanya dilepas saat tidak ada jari virtual yang menekan.
- Tidak ada pemanggilan `dumpsys`, `ps`, atau proses anak lain di jalur polling.
- Saat target sudah terdeteksi foreground, pemindaian `/proc` dilewati sampai status foreground
  berubah. Pemindaian penuh dilakukan sekali saat transisi, sehingga PID yang dipakai ulang tidak
  terlewat.

## Instalasi

1. Ambil `Flick-v*.zip` dari artifact GitHub Actions (selalu bersama `.zip.sha256`), atau build
   sendiri.
2. Verifikasi paket: `sha256sum -c Flick-v*.zip.sha256`.
3. Di AxManager, buka **Plugins** lalu install zip tersebut.
4. Buka WebUI Flick.
5. Atur sensitivitas dan opsi lain, lalu tekan **Aktifkan**.
6. Status akan menjadi `active` saat package target di foreground, dan `idle` saat tidak.
7. Gunakan **Hentikan darurat** untuk mengembalikan sentuhan normal.

Plugin ini membutuhkan AxManager dengan server version minimal `19200` (`axeronPlugin=19200`
pada `module.prop`). Flick berjalan dengan hak akses shell dan tidak membutuhkan root.

## Build

Workflow GitHub Actions berjalan pada setiap branch dan pull request. Packaging tidak akan dimulai
sebelum semua pemeriksaan ini lulus:

- selftest host (`-O2`) dan selftest host dengan ASan + UBSan;
- unit test deteksi foreground dengan `/proc` palsu;
- microbenchmark jalur matematika `-O2` vs `-Os`;
- cross-build selftest untuk `arm64` dan `arm`;
- `shellcheck` dan `sh -n` untuk semua script;
- `node --check` untuk JavaScript di `index.html`;
- build daemon `arm64` + `arm`, verifikasi ELF/PIE/RELRO/BIND_NOW/16 KB, lalu packaging deterministik
  dengan `SHA256SUMS`.

Artifact `Flick` berisi `Flick-v*.zip`, checksum, manifest, dan `build-info.txt`. Detail hasil
pengukuran ada di [`docs/VERIFICATION.md`](docs/VERIFICATION.md).

Reproduksi build secara lokal:

```bash
export NDK=$ANDROID_NDK_HOME
BIN=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
mkdir -p out/system/bin out/webroot

COMMON=(
  -std=c++17 -Os -DNDEBUG -D_FORTIFY_SOURCE=2
  -ffp-contract=off -fno-exceptions -fno-rtti -fvisibility=hidden
  -fstack-protector-strong -fno-unwind-tables -fno-asynchronous-unwind-tables
  -ffunction-sections -fdata-sections -ffile-prefix-map="$PWD"=.
  -Wall -Wextra -Werror -Wformat=2 -Wshadow -Wundef -Wcast-align -Wpointer-arith
  -pie -nostdlib++
  -Wl,--gc-sections -Wl,--as-needed -Wl,--build-id=none
  -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wl,-z,max-page-size=16384
)
for t in aarch64-linux-android24:arm64 armv7a-linux-androideabi24:arm; do
  "$BIN/${t%%:*}-clang++" "${COMMON[@]}" flick.cpp \
    -o "out/system/bin/flick.${t##*:}" -lm
done

cp module.prop customize.sh uninstall.sh hw.sh ctl.sh notify.sh out/
cp index.html out/webroot/
python3 tools/package.py --root out --out dist
(cd dist && sha256sum -c Flick-*.zip.sha256)
```

Selftest dan pemeriksaan host:

```bash
g++ -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -Werror \
  -fno-exceptions -fno-rtti selftest.cpp -o selftest-host
./selftest-host

g++ -std=c++17 -O2 -ffp-contract=off -Wall -Wextra -Werror \
  -DFLICK_PROC_TEST -DFLICK_PROC_ROOT='"/tmp/flick-proc-test"' \
  proc_test.cpp -o proc-test
./proc-test

python3 tools/check_webui.py index.html
```

`tools/package.py` memakai urutan file dan timestamp tetap, sehingga commit yang sama menghasilkan
zip yang identik. Timestamp zip mengikuti perubahan terakhir `module.prop`, jadi commit dokumentasi
atau CI tidak mengubah hash artifact. Binary `arm64` dan `arm` disejajarkan ke 16 KB untuk perangkat
Android dengan page size 16 KB, dan tetap kompatibel dengan perangkat 4 KB. Minimum Android tetap
API 24.

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
| `proc_test.cpp` | Unit test deteksi foreground dan pemindaian `/proc` |
| `index.html` | WebUI plugin |
| `ctl.sh` | Entry point WebUI: `status`, `save`, `start`, `stop` |
| `customize.sh` | Menyalin binary sesuai ABI saat instalasi |
| `uninstall.sh` | Menghentikan daemon dan mengembalikan properti |
| `hw.sh` | Mengubah dan mengembalikan `debug.sf.hw` |
| `notify.sh` | Notifikasi status aktif/idle |
| `module.prop` | Metadata plugin AxManager |
| `tools/` | Packager deterministik, pemeriksa WebUI, dan microbenchmark |
| `docs/` | Rencana optimasi dan hasil verifikasi |

## Kredit

Kalkulasi sensitivitas (penggabungan sumbu X/Y, kurva, dan parameter filter) mengikuti fitur
**Magic Touch** di aplikasi **Game Corner** milik **FahrezONE**, yang diperoleh melalui reverse
engineering sebagaimana dicatat di `flick.cpp`. Bagian daemon, grab/uinput, deteksi foreground,
konfigurasi, dan WebUI merupakan bagian dari proyek ini.

## Catatan

- Efek dan kompatibilitas bergantung pada perangkat dan game.
- HW Touch Enhancer memakai properti debug lama `debug.sf.hw`; efeknya bergantung pada perangkat dan
  `setprop` bisa ditolak di shell mode. Fitur ini opsional dan nilai sebelumnya dikembalikan saat
  Flick berhenti.
- Binary disejajarkan untuk page size 16 KB (Android 15+), minimum API 24.
- WebUI memakai `moduleInfo().moduleDir` dari AxManager. Jalur shell mode
  `/data/user_de/0/com.android.shell/axeron/plugins/flick` hanya cadangan bila host tidak
  menyediakan `moduleDir`.
- Belum ada lisensi.
