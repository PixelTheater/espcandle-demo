# Convenience targets for the ESP Candle ESPHome firmware.
#
# Pin the ESPHome version this project is tested against. Override on the
# command line to test other versions, e.g.:
#
#   make config ESPHOME=esphome@dev
#   make run    ESPHOME=esphome@2026.6 DEVICE=candle-test
#
# Install parallel versions with pipx:
#
#   pipx install esphome
#   pipx install --suffix=@2026.6 esphome==2026.6.5
#   pipx install --suffix=@dev       esphome==dev

ESPHOME ?= esphome@2026.6.5
DEVICE  ?= candle-test
YAML     = $(DEVICE).yaml
FACTORY_YAML := espcandle.factory.factory.yaml
ALL_CONFIGS := espcandle.base.yaml espcandle.example.yaml espcandle.factory.yaml espcandle.factory.factory.yaml

.PHONY: version config compile run logs clean factory-config factory factory-run smoke-test

version:
	@$(ESPHOME) version

config:
	$(ESPHOME) config $(YAML)

compile:
	$(ESPHOME) compile $(YAML)

run:
	$(ESPHOME) run $(YAML)

logs:
	$(ESPHOME) logs $(YAML)

clean:
	rm -rf .esphome

# Factory image (bench flash / web flasher). Compiles the
# espcandle.factory.factory.yaml, which packages the runtime-PSK
# factory config plus http_request OTA self-update.
factory-config:
	$(ESPHOME) config $(FACTORY_YAML)

factory: factory-config
	$(ESPHOME) compile $(FACTORY_YAML)

factory-run: factory
	$(ESPHOME) run $(FACTORY_YAML)

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
