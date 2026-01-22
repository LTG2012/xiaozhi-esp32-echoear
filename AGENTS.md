# AGENTS.md

## Project overview
- ESP-IDF (CMake) firmware for the XiaoZhi ESP32 chatbot.
- Main firmware component lives in `main/` with C++ sources (`.cc`) and headers (`.h`).
- Board-specific implementations live in `main/boards/<board-name>/` with `config.json`, `config.h`, and a board `.cc`.
- Assets and localization live under `main/assets/` (generated header: `main/assets/lang_config.h`).
- Python tooling lives under `scripts/` (release packaging, assets generation, etc.).
- Partition tables live in `partitions/`.

## Tooling and environment
- ESP-IDF version: `>= 5.4.0` (see `main/idf_component.yml`).
- Build system: CMake via ESP-IDF `idf.py`.
- This repo uses Google C++ style (see `README.md`).
- No Cursor rules or Copilot rules were found (`.cursor/rules/`, `.cursorrules`, `.github/copilot-instructions.md`).

## Build commands (ESP-IDF)
Run these from repo root with the ESP-IDF environment activated.

- Set target chip (once per board/chip):
  - `idf.py set-target esp32s3`
  - `idf.py set-target esp32c3`
  - `idf.py set-target esp32c6`
  - `idf.py set-target esp32p4`
- Clean build:
  - `idf.py fullclean`
- Configure:
  - `idf.py menuconfig`
- Build:
  - `idf.py build`
- Flash and monitor:
  - `idf.py flash monitor`
  - `idf.py build flash monitor`
- Merge binaries (some board READMEs):
  - `idf.py merge-bin`

## Board packaging (release script)
If a board directory contains `config.json`, use the packaging script:

- `python scripts/release.py <board-dir-name>`
- Some boards pass `-c <config.json>` for alternate configs (see board READMEs).

## Assets tooling
These scripts are invoked by the build or used manually for assets creation.

- Default assets build invoked by CMake:
  - `python scripts/build_default_assets.py ...`
- SPIFFS assets tooling:
  - `python scripts/spiffs_assets/build.py ...`

## Linting / formatting
- No lint or formatter configuration was found in the repo.
- Follow Google C++ style manually; keep formatting consistent with existing files.
- If you add a formatter, keep it scoped and document it in this file.

## Tests
- No dedicated test sources or test targets were found in this repo.
- ESP-IDF Unity test runner is enabled in `sdkconfig` (`CONFIG_UNITY_ENABLE_IDF_TEST_RUNNER=y`).
- If Unity tests are added later, use the standard ESP-IDF test runner commands
  (for example, `idf.py -T <test_filter> flash monitor` per ESP-IDF docs).

## Code style guidelines (C/C++)
Follow the patterns in `main/application.cc`, `main/main.cc`, and board files.

### Files and layout
- Prefer `.cc` for C++ and `.c` for C sources; headers are `.h`.
- Keep one primary class per file when practical.
- Use include guards with uppercase names (example: `#ifndef _APPLICATION_H_`).
- Keep file-level comments minimal; add comments only for non-obvious logic.

### Includes
- Order includes as:
  1) Local project headers (`"application.h"`)
  2) Third-party/ESP-IDF headers (`<esp_log.h>`, `<freertos/...>`)
  3) Standard library headers (`<string>`, `<memory>`)
- Use quoted includes for project headers and angle brackets for external ones.

### Naming
- Types and classes: `UpperCamelCase` (example: `AudioService`).
- Functions/methods: `UpperCamelCase` (example: `GetDisplay`).
- Variables: `lower_snake_case`.
- Private members: `lower_snake_case_` trailing underscore.
- Enum values use a `k` prefix (`kDeviceStateIdle`).
- Macros and compile-time flags: `UPPER_SNAKE_CASE`.

### Formatting
- Indentation: 4 spaces.
- Braces on the same line as the declaration.
- Align initializer lists and wrap long parameter lists as in existing files.
- Keep line length reasonable; wrap long log strings and conditions.

### Types and ownership
- Prefer `std::unique_ptr` for ownership and `std::shared_ptr` only when shared.
- Use references (`T&`) for non-nullable parameters; pointers when null is valid.
- Use `std::string_view` for read-only string parameters when possible.

### Error handling and logging
- Use ESP-IDF error types (`esp_err_t`) and `ESP_ERROR_CHECK` for fatal errors.
- Log with `ESP_LOGI/W/E` and a `TAG` macro per file.
- Return `false`/`nullptr` for recoverable failures; log context first.

### Concurrency and callbacks
- Use FreeRTOS primitives (`EventGroupHandle_t`, tasks) as in `main/application.cc`.
- Callbacks capture `this` carefully and avoid heavy work on ISR or timer context.
- Use `Schedule(...)` for main-task thread safety when updating UI or protocol state.

### Configuration and build-time flags
- Board selection is driven by Kconfig and `main/Kconfig.projbuild`.
- Add new board types to both `main/Kconfig.projbuild` and `main/CMakeLists.txt`.
- Avoid editing generated files like `main/assets/lang_config.h` directly.

### JSON and protocol handling
- Use `cJSON` helpers for JSON parsing/creation.
- Validate JSON types with `cJSON_IsString`/`cJSON_IsObject` before use.
- Prefer `cJSON_PrintUnformatted` for log payloads.

## Common workflows
### Add a new board
- Create a new `main/boards/<board-name>/` folder with `config.h`, `config.json`, and a board `.cc`.
- Add a new `BOARD_TYPE_*` entry in `main/Kconfig.projbuild`.
- Register the board in the `main/CMakeLists.txt` board selection block.
- Use `python scripts/release.py <board-name>` to build a packaged firmware.

### Build a specific board
- Use `idf.py set-target <chip>` and `idf.py menuconfig` to select board type.
- Ensure `sdkconfig` or the board `config.json` matches flash/partition settings.

## Notes for agents
- This repo contains many board variants; inspect the closest existing board
  implementation before adding new hardware support.
- When touching display/audio/board code, watch for power-save and memory usage.
