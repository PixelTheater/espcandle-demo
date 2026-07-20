# Publishing firmware to support.soliddifference.com

The support site's **Firmware Updates** page
(<https://support.soliddifference.com/firmware/>) offers browser-based flashing
(ESP Web Tools) plus manual `.bin` downloads. It is fully data-driven: a GitHub
Actions workflow on the support repo (`sync-firmware.yml`) pulls binaries from a
product repo's GitHub Release, generates the ESP Web Tools manifest, updates the
release history, and redeploys the site.

For the ESP Candle to appear there, **this repo must publish releases in the
shape that workflow expects.** The support site owns the manifest + page markup;
this repo owns the binaries and the notification. This document is the contract
between the two.

> **Status (2026-07-20):** the support side is **deployed and live** at
> <https://support.soliddifference.com/firmware/> (commit `401a8c1`). The Candle
> card is in "coming soon" (flash buttons hidden); manifests 404 until the first
> sync. Auto-activates when this repo publishes a release and fires the dispatch
> below. Nothing on the support side blocks you.

The Candle will be listed with **two firmware options**, matching the SIEBENUHR
products:

- **ESPHome** — the primary Home Assistant path (`espcandle.factory.factory.yaml`)
- **Standalone** — the optional PlatformIO demo firmware (`platformio.ini`, `env:esp32s3`)

---

## What you must produce, per release

### 1. Two full-flash (merged, offset-0) binaries

ESP Web Tools flashes a **single image starting at flash offset 0** — bootloader
+ partition table + app merged into one file. A bare app image (PlatformIO's
`firmware.bin`, which lives at `0x10000`) will **not** boot when flashed at 0.

The Candle is an **ESP32-S3, 4 MB flash, `qio` @ 80 MHz** (see
`boards/espcandle.json`). Name the two outputs exactly:

| Firmware  | Filename                                     | How to build |
|-----------|----------------------------------------------|--------------|
| ESPHome   | `firmware-espcandle-esphome-<version>.bin`   | `make factory` → `firmware.factory.bin` |
| Standalone| `firmware-espcandle-<version>.bin`           | `pio run -e esp32s3` → `firmware.factory.bin` |

`<version>` is the release version **without** a `v` prefix (e.g. `0.2.0`).

Both toolchains already emit a merged, offset-0 `firmware.factory.bin` (ESPHome
via its factory build; pioarduino via `esptool merge-bin` during `pio run`).
Copy and rename — do not publish the bare `firmware.bin` (app at `0x10000`).

#### ESPHome image

```bash
make factory   # compiles espcandle.factory.factory.yaml (node name: espcandle)
# ESPHome 2026.x (IDF):
#   esphome/.esphome/build/espcandle/build/firmware.factory.bin
# Older PlatformIO layout (still possible locally):
#   esphome/.esphome/build/espcandle/.pioenvs/espcandle/firmware.factory.bin
cp <path-above> firmware-espcandle-esphome-<version>.bin
```

The node name is `espcandle` (from `esphome.name` in the factory YAML). The
publish workflow checks both layouts.

#### Standalone demo image

```bash
pio run -e esp32s3
# -> .pio/build/esp32s3/firmware.factory.bin  (merged: bootloader + partitions + boot_app0 + app)
cp .pio/build/esp32s3/firmware.factory.bin firmware-espcandle-<version>.bin
```

> Optional local check — flash the merged image at `0x0`:
> `esptool --chip esp32s3 write_flash 0x0 firmware-espcandle-<version>.bin`
>
> If you ever need to merge by hand (older PlatformIO without
> `firmware.factory.bin`), the Arduino ESP32-S3 defaults are:
> `0x0 bootloader.bin`, `0x8000 partitions.bin`,
> `0xe000 …/boot_app0.bin`, `0x10000 firmware.bin`.

### 2. A GitHub Release with those two assets

- Tag the release **`v<version>`** (e.g. `v0.2.0`).
- Attach **both** `.bin` files as release assets, named exactly as above.
- Put user-facing release notes in the release body (the support site shows them
  verbatim in the Release History table).

> **Version story — keep three things in sync.** The support site shows the
> **GitHub Release tag** (`<version>` above). Source markers must match it:
> `README.md` frontmatter `version`, and ESPHome `project_version` in
> `espcandle.base.yaml`. For `0.2.0` those are already aligned in-tree; create
> the `v0.2.0` tag/release to publish. Mismatched versions are the most likely
> source of confusion here.

### 3. A notification (`repository_dispatch`) to the support repo

After the release publishes, fire a `repository_dispatch` at the support repo so
it syncs and redeploys. The support workflow is keyed on this payload:

```jsonc
{
  "event_type": "firmware-release",
  "client_payload": {
    "product":       "espcandle",          // required — selects this product
    "version":       "0.2.0",              // no 'v' prefix
    "release_date":  "2026-07-20",         // YYYY-MM-DD
    "release_notes": "Short summary shown in the release table"
  }
}
```

One dispatch per release covers **both** firmware types — the support workflow
pulls both `firmware-espcandle-esphome-<version>.bin` and
`firmware-espcandle-<version>.bin` from the same `v<version>` release.

> **Two secrets, two directions — don't conflate them:**
>
> - **`SUPPORT_SITE_PAT`** (on **this** repo) lets the candle *dispatch* to the
>   support repo. This repo is under `PixelTheater`, the support site under
>   `soliddifference`; the built-in `GITHUB_TOKEN` **cannot** dispatch across
>   orgs. Create a fine-grained PAT with the support repo's
>   **Metadata: read + Contents: read/write** (repository_dispatch rides on
>   Contents), store it here as `SUPPORT_SITE_PAT`, and use it below.
> - **`CANDLE_REPO_TOKEN`** (on the **support** repo) is only needed **if this
>   repo is private** — the support workflow reads the release assets with
>   `GITHUB_TOKEN`, which can read *public* cross-org repos but not private ones.
>   If `espcandle-demo` is private, the support maintainer adds a PAT with
>   `contents: read` on this repo as `CANDLE_REPO_TOKEN`. If this repo is public,
>   nothing is needed.

---

## Release workflow

`.github/workflows/publish-firmware.yml` runs on `release: [published]` (and
`workflow_dispatch` for republish). It builds both factory images, uploads the
renamed assets to the release, and fires the dispatch above.

**One-time setup:** add repo secret `SUPPORT_SITE_PAT` (fine-grained PAT on
`soliddifference/support.soliddifference.com` with Contents read/write — see
above). Without it, the Notify step fails after the bins are already uploaded.

**To ship `v0.2.0`:**

1. Merge the version + workflow changes to `main`.
2. Confirm `SUPPORT_SITE_PAT` is set.
3. Create the release (draft first is fine; publishing triggers the workflow):

```bash
gh release create v0.2.0 --title "0.2.0" --notes-file - <<'EOF'
ESP Candle 0.2.0 — lean ESPHome factory firmware (Preset + Candle/Color/Magic),
standalone PlatformIO demo, and support-site web flasher integration.
See CHANGELOG.md for the full list.
EOF
```

4. Watch **Actions → Publish firmware to support site**, then verify:

```bash
curl -s -o /dev/null -w "%{http_code}\n" \
  https://support.soliddifference.com/firmware/manifest-espcandle-esphome.json
curl -s https://support.soliddifference.com/firmware/releases-espcandle.json
```

---

## One-time assets the support site needs from you

Independent of the per-release automation, the support page needs a product
image and a store link to render the Candle's card.

- **Product photo** — ✅ done. `media/ESPCandle-product.jpg` has been copied into
  the support repo as `content/images/espcandle-product.jpg`.
- **Store URL** — ⚠️ still needed. The card currently links to the store
  homepage (`soliddifference.com/store`). Send the Candle's specific product-page
  URL (SIEBENUHR cards link to `soliddifference.com/store/p/...`) so it can be
  swapped in.

## Checklist

- [x] Reconcile version markers (README `0.2.0` / ESPHome `project_version` `0.2.0`)
- [x] Publish workflow (`.github/workflows/publish-firmware.yml`)
- [x] Repo is public — no `CANDLE_REPO_TOKEN` needed on the support repo
- [x] Product photo handed over (already in support repo)
- [ ] `SUPPORT_SITE_PAT` secret created on this repo (cross-org PAT)
- [ ] GitHub Release `v0.2.0` published (workflow uploads bins + dispatches)
- [ ] Support-site manifests return 200; Candle card leaves "coming soon"
- [ ] Candle store-page URL sent to the support-site maintainer
