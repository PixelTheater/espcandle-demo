# Root convenience wrappers — real targets live in esphome/Makefile.
#
#   make smoke-test     # validate YAMLs + compile factory (no hardware)
#   make factory-run    # compile, flash, and monitor factory image
#
# Uses .venv/bin/esphome when present; override with ESPHOME=...

ESPHOME ?= $(CURDIR)/.venv/bin/esphome

.PHONY: smoke-test factory factory-run factory-config version

smoke-test factory factory-run factory-config version:
	$(MAKE) -C esphome ESPHOME="$(ESPHOME)" $@
