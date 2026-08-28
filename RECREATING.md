# Recreating MacroZ

The archive contains the source and pinned dependency manifests, but not downloaded dependencies or
generated output. Run the following commands from the extracted project directory.

## Firmware

Install Git, Python 3, `west`, CMake, Ninja, devicetree compiler, and Zephyr SDK 0.16.9. Then create
an isolated west workspace so this repository's `zephyr/` module metadata does not conflict with the
Zephyr source project:

```sh
PROJECT="$PWD"
WORKSPACE="$(mktemp -d)"
mkdir "$WORKSPACE/config"
cp -R "$PROJECT/config/." "$WORKSPACE/config/"
cd "$WORKSPACE"
west init -l config
west update
west zephyr-export
west build -s zmk/app -b nice_nano//zmk -- \
  -DSHIELD=labib_macropad \
  -DZMK_CONFIG="$WORKSPACE/config" \
  -DZMK_EXTRA_MODULES="$PROJECT"
```

The flashable firmware is written to `build/zephyr/zmk.uf2`. The pinned ZMK revision is declared in
`config/west.yml`.

## Web Configurator

Install a current Node.js release and enable Corepack, then install exactly the dependencies from the
lockfile and create the production bundle:

```sh
cd web
corepack enable
pnpm install --frozen-lockfile
pnpm run build
```

The production site is written to `web/dist/`. Use `pnpm run dev` instead for the development server.

## GitHub Builds

Push the extracted project to a GitHub repository and enable Actions. The workflow in
`.github/workflows/build-firmware.yml` uses `build.yaml` and publishes the firmware as
`labib-macropad-nice-nano-v2.uf2`.

## Recreate The Source Archive

From the directory containing the project folder, run:

```sh
zip -r macroz-labib-macropad.zip macroz \
  -x 'macroz/web/node_modules/*' \
     'macroz/web/node_modules/' \
     'macroz/web/dist/*' \
     'macroz/web/dist/' \
     'macroz/.west/*' \
     'macroz/.west/' \
     'macroz/build/*' \
     'macroz/build/' \
     'macroz/modules/*' \
     'macroz/modules/' \
     'macroz/tools/*' \
     'macroz/tools/' \
     'macroz/bootloader/*' \
     'macroz/bootloader/' \
     'macroz/.venv/*' \
     'macroz/.venv/' \
     'macroz/firmware/*' \
     'macroz/firmware/' \
     'macroz/zmk-config.zip'
```

These exclusions contain downloaded dependencies, generated output, local workspaces, or the original
hardware reference archive. None is required to rebuild the project.
