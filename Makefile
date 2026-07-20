# Root convenience wrappers — real ESPHome targets live in esphome/Makefile.
#
#   make smoke-test     # validate YAMLs + compile factory (no hardware)
#   make factory-run    # compile, flash, and monitor factory image
#
# ESPHOME defaults to .venv/bin/esphome when present, otherwise `esphome` on
# PATH (CI). Override: make smoke-test ESPHOME=esphome@2026.6.5

ESPHOME ?= $(if $(wildcard $(CURDIR)/.venv/bin/esphome),$(CURDIR)/.venv/bin/esphome,esphome)

.PHONY: smoke-test factory factory-run factory-config version

smoke-test factory factory-run factory-config version:
	$(MAKE) -C esphome ESPHOME="$(ESPHOME)" $@
