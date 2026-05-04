# Convenience targets for the ESP Candle ESPHome firmware.
#
# Pin the ESPHome version this project is tested against. Override on the
# command line to test other versions, e.g.:
#
#   make config ESPHOME=esphome@dev
#   make run    ESPHOME=esphome@2025.7 DEVICE=candle-test
#
# Install parallel versions with pipx:
#
#   pipx install esphome
#   pipx install --suffix=@2025.7 esphome==2025.7.3
#   pipx install --suffix=@dev    esphome==dev

ESPHOME ?= esphome@2026.4
DEVICE  ?= candle-test
YAML     = $(DEVICE).yaml

.PHONY: version config compile run logs clean

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
