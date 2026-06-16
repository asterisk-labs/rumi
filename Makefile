VERSION := $(shell tr -d '[:space:]' < VERSION)

SRC_DIR        := core
BUILD_DIR      := core/build
TSAN_BUILD_DIR := core/build-tsan
PYTHON         ?= python
PREFIX         ?= /usr/local

# Bindings. Python exists; R is planned (terra, system GDAL, no vendoring).
PY_DIR     ?= bindings/python
R_DIR      ?= bindings/r
PY_LIB_DIR := $(PY_DIR)/rumi/_lib

ifeq ($(shell uname -s),Darwin)
LIB_NAME := librumi.dylib
else ifeq ($(OS),Windows_NT)
LIB_NAME := rumi.dll
else
LIB_NAME := librumi.so
endif

# Homebrew prefix on macOS so find_package picks up GDAL.
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW_PREFIX),)
PREFIX_FLAG := -DCMAKE_PREFIX_PATH=$(BREW_PREFIX)
endif

CMAKE_BUILD_TYPE ?= Release
CMAKE_FLAGS      ?=

# The standalone lib that the Python binding loads is requested explicitly here.
CMAKE_OPTS := \
	-G Ninja \
	-DCMAKE_BUILD_TYPE=$(CMAKE_BUILD_TYPE) \
	-DRUMI_BUILD_SHARED_LIB=ON \
	$(PREFIX_FLAG) \
	$(CMAKE_FLAGS)

.PHONY: all build configure lib stage-lib sync \
        python r install clean help

# The full flow for now. Wipe, then build and run every binding. The Python
# suite runs inside make python (pytest under bindings/python/tests).
all:
	$(MAKE) clean
	$(MAKE) sync
	$(MAKE) python
	$(MAKE) r
	@echo "rumi $(VERSION): all green"

$(BUILD_DIR)/CMakeCache.txt:
	cmake -S $(SRC_DIR) -B $(BUILD_DIR) $(CMAKE_OPTS)

configure:
	cmake -S $(SRC_DIR) -B $(BUILD_DIR) $(CMAKE_OPTS)

build: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR)

# Just the shared lib, staged next to the Python binding. Single CI entry.
lib: build
	@mkdir -p $(PY_LIB_DIR)
	@rm -f $(PY_LIB_DIR)/librumi* $(PY_LIB_DIR)/rumi*.dll
	@lib=$$(find $(BUILD_DIR) \( -name 'librumi*.dylib' -o -name 'librumi*.so*' -o -name 'rumi*.dll' \) | head -1); \
	  [ -n "$$lib" ] || { echo "no built librumi under $(BUILD_DIR)"; exit 1; }; \
	  cp -a "$$(dirname "$$lib")"/librumi* $(PY_LIB_DIR)/ 2>/dev/null || true; \
	  cp -a "$$(dirname "$$lib")"/rumi*.dll $(PY_LIB_DIR)/ 2>/dev/null || true; \
	  echo "staged lib into $(PY_LIB_DIR)"

# Copy the lib to STAGE_DIR for upload-artifact, no glob logic in the YAML.
STAGE_DIR ?= staged
stage-lib: lib
	@mkdir -p $(STAGE_DIR)
	@cp -a $(PY_LIB_DIR)/librumi* $(STAGE_DIR)/ 2>/dev/null || true
	@cp -a $(PY_LIB_DIR)/rumi*.dll $(STAGE_DIR)/ 2>/dev/null || true
	@ls -1 $(STAGE_DIR)

# Python and CMake read VERSION at build time, so only a hardcoded manifest
# needs rewriting. That is R's DESCRIPTION, written here when bindings/r/
# exists. Today this is a no-op guard that fails on a malformed VERSION.
sync:
	@printf '%s' "$(VERSION)" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$$' \
	  || { echo "VERSION '$(VERSION)' is not X.Y.Z[-prerelease]"; exit 1; }
	@if [ -f $(R_DIR)/DESCRIPTION ]; then \
	  sed -i.bak -E 's/^Version:.*/Version: $(VERSION)/' $(R_DIR)/DESCRIPTION; \
	  rm -f $(R_DIR)/DESCRIPTION.bak; \
	  v=$$(grep -E '^Version:' $(R_DIR)/DESCRIPTION | sed 's/Version:[[:space:]]*//'); \
	  [ "$$v" = "$(VERSION)" ] || { echo "sync check: DESCRIPTION=$$v != $(VERSION)"; exit 1; }; \
	  echo "sync OK $(VERSION) (python dynamic, R DESCRIPTION written)"; \
	else \
	  echo "sync OK $(VERSION) (python dynamic, no R binding yet)"; \
	fi

# Build the lib, install the binding editable so its metadata exists, smoke-load
# it, then run pytest if it ships tests.
python: lib
	@$(PYTHON) -c 'import numpy, cffi' 2>/dev/null || { echo "missing deps: pip install numpy cffi"; exit 1; }
	$(PYTHON) -m pip install -e $(PY_DIR) -q
	cd $(PY_DIR) && $(PYTHON) -c "import rumi; print('loaded rumi', rumi.__version__)"
	@if [ -d $(PY_DIR)/tests ] && $(PYTHON) -c 'import pytest' 2>/dev/null; then \
	  cd $(PY_DIR) && $(PYTHON) -m pytest -q; rc=$$?; \
	  [ $$rc -eq 0 ] || [ $$rc -eq 5 ] || exit $$rc; \
	else \
	  echo "no python tests (or pytest missing); skipping"; \
	fi

# R binding (terra, system GDAL). Skips cleanly until bindings/r/ exists.
r:
	@if [ ! -d $(R_DIR) ]; then \
	  echo "no R binding yet ($(R_DIR) absent); skipping"; \
	else \
	  command -v R >/dev/null || { echo "missing R"; exit 1; }; \
	  ( cd $(R_DIR) && Rscript -e 'if (requireNamespace("roxygen2", quietly=TRUE)) roxygen2::roxygenise()' ); \
	  R CMD INSTALL $(R_DIR); \
	  R CMD build $(R_DIR); \
	fi

install: build
	cmake --install $(BUILD_DIR) --prefix $(PREFIX)

clean:
	rm -rf $(BUILD_DIR) $(TSAN_BUILD_DIR) $(STAGE_DIR)
	rm -f $(PY_LIB_DIR)/librumi* $(PY_LIB_DIR)/rumi*.dll

help:
	@echo "Targets:"
	@echo "  all             clean + sync + python + r"
	@echo "  build           incremental build"
	@echo "  configure       force CMake reconfigure"
	@echo "  lib             build the shared lib, stage it into the binding (CI entry)"
	@echo "  stage-lib       copy the lib into STAGE_DIR for upload-artifact"
	@echo "  sync            validate VERSION; write R DESCRIPTION if present"
	@echo "  python          build lib, install editable, smoke-load, pytest"
	@echo "  r               build the R binding (skips until $(R_DIR) exists)"
	@echo "  install         cmake --install into PREFIX"
	@echo "  clean           remove build dirs and staged libs"
	@echo ""
	@echo "Overrides:"
	@echo "  CMAKE_BUILD_TYPE=Debug   PYTHON=python3.12   PREFIX=/opt/local"
	@echo "  STAGE_DIR=out   PY_DIR=bindings/python   R_DIR=bindings/r"