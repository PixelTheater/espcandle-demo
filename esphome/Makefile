# Convenience targets for the ESP Candle ESPHome firmware.
#
# Default ESPHOME is `esphome` on PATH. The root Makefile prefers
# `.venv/bin/esphome` when present. Override to test other versions:
#
#   make config ESPHOME=esphome@dev
#   make run    ESPHOME=esphome@2026.6 DEVICE=candle-test
#
# Install parallel versions with pipx:
#
#   pipx install esphome
#   pipx install --suffix=@2026.6 esphome==2026.6.5
#   pipx install --suffix=@dev       esphome==dev

ESPHOME ?= esphome
DEVICE  ?= candle-test
YAML     = $(DEVICE).yaml
# Top Board default for a fresh flash (v1.3, v1.4 or v1.6), e.g.
#   make factory-run TOP=v1.6
# A candle that already stored a Top Board choice keeps it.
TOP     ?=
SUBS     = $(if $(TOP),-s top_board $(TOP))
FACTORY_YAML := espcandle.factory.factory.yaml
ALL_CONFIGS := espcandle.base.yaml espcandle.example.yaml espcandle.factory.yaml espcandle.factory.factory.yaml

.PHONY: version config compile run logs clean factory-config factory factory-run smoke-test

version:
	@$(ESPHOME) version

config:
	$(ESPHOME) $(SUBS) config $(YAML)

compile:
	$(ESPHOME) $(SUBS) compile $(YAML)

run:
	$(ESPHOME) $(SUBS) run $(YAML)

logs:
	$(ESPHOME) logs $(YAML)

clean:
	rm -rf .esphome

# Factory image (bench flash / web flasher). Compiles
# espcandle.factory.factory.yaml (same as factory until HTTP
# self-update is re-enabled with a published manifest).
factory-config:
	$(ESPHOME) $(SUBS) config $(FACTORY_YAML)

factory: factory-config
	$(ESPHOME) $(SUBS) compile $(FACTORY_YAML)

factory-run: factory
	$(ESPHOME) $(SUBS) run $(FACTORY_YAML)

# Automated smoke test (no hardware required). Validates every config and
# compiles the factory binary. Use before pushing or opening a PR. For the
# on-device runbook, see docs/testing.md.
smoke-test:
	@echo "==> config validation"
	@for f in $(ALL_CONFIGS); do \
		$(ESPHOME) config $$f >/dev/null && echo "  OK: $$f" || { echo "  FAIL: $$f"; exit 1; }; \
	done
	@echo "==> compile factory image"
	$(ESPHOME) compile $(FACTORY_YAML) && echo "  OK: compile"
	@echo "==> smoke-test passed"
