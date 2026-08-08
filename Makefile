# make            build, stage the lib, editable install, smoke load
# make build      build librumi and the GDAL plugin only
# make lib        build and stage the shared lib next to the binding (CI entry)
# make test       build, install, then pytest
# make r          build the R binding (skips until bindings/r exists)
# make sync       validate VERSION; write R DESCRIPTION if present
# make install    cmake --install into PREFIX (/usr/local)
# make clean      remove all build output, caches and generated files
# make submodules fetch or update geozl (and OpenZL under it)

# rumi build. `make help` lists the targets and the variables.
# Vendors geozl as a submodule, fetched on first build. GDAL is a system
# dependency, 3.8 or newer, found by find_package.

PYTHON ?= python
PREFIX ?= /usr/local
BUILD  ?= Release
GEN    ?= Ninja
PLUGIN ?= ON

CORE      := core
BUILD_DIR := core/build
GEOZL     := extern/geozl
PY_DIR    := bindings/python
R_DIR     := bindings/r
PY_LIB_DIR := $(PY_DIR)/rumi/_lib
STAGE_DIR ?= staged
UNAME     := $(shell uname -s)
VERSION   := $(shell tr -d '[:space:]' < VERSION)

ifeq ($(UNAME),Darwin)
  LIBRUMI := librumi.dylib
else ifeq ($(OS),Windows_NT)
  LIBRUMI := rumi.dll
else
  LIBRUMI := librumi.so
endif

# Homebrew prefix on macOS so find_package picks up GDAL.
BREW_PREFIX := $(shell brew --prefix 2>/dev/null)
ifneq ($(BREW_PREFIX),)
  PREFIX_FLAG := -DCMAKE_PREFIX_PATH=$(BREW_PREFIX)
endif

# RUMI_BUILD_SHARED_LIB is on because the Python binding dlopens that lib; the
# plugin alone is what GDAL auto-loads and carries no C API.
CMAKE_FLAGS ?=
CMAKE_OPTS  := -G $(GEN) -DCMAKE_BUILD_TYPE=$(BUILD) \
               -DRUMI_BUILD_SHARED_LIB=ON -DRUMI_BUILD_PLUGIN=$(PLUGIN) \
               $(PREFIX_FLAG) $(CMAKE_FLAGS)

.PHONY: all build configure lib stage-lib python test r sync \
        install submodules clean help

all: python

# A fresh clone has an empty submodule, fetch it so a bare make works.
$(GEOZL)/core/CMakeLists.txt:
	git submodule update --init --recursive

submodules:
	git submodule update --init --recursive

$(BUILD_DIR)/CMakeCache.txt: $(GEOZL)/core/CMakeLists.txt
	cmake -S $(CORE) -B $(BUILD_DIR) $(CMAKE_OPTS)

configure: $(GEOZL)/core/CMakeLists.txt
	cmake -S $(CORE) -B $(BUILD_DIR) $(CMAKE_OPTS)

build: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR)

# Stage the shared lib next to the binding, cffi loads it from there.
lib: build
	@mkdir -p $(PY_LIB_DIR)
	@rm -f $(PY_LIB_DIR)/librumi* $(PY_LIB_DIR)/rumi*.dll
	@f=$$(find $(BUILD_DIR) \( -name 'librumi*.dylib' \
	        -o -name 'librumi*.so*' -o -name 'rumi*.dll' \) | head -1); \
	  [ -n "$$f" ] || { echo "no $(LIBRUMI) under $(BUILD_DIR)"; exit 1; }; \
	  cp -a "$$(dirname "$$f")"/librumi* $(PY_LIB_DIR)/ 2>/dev/null || true; \
	  cp -a "$$(dirname "$$f")"/rumi*.dll $(PY_LIB_DIR)/ 2>/dev/null || true; \
	  echo "staged $(LIBRUMI) into $(PY_LIB_DIR)"

# Copy the lib to STAGE_DIR for upload-artifact, no glob logic in the YAML.
stage-lib: lib
	@mkdir -p $(STAGE_DIR)
	@cp -a $(PY_LIB_DIR)/librumi* $(STAGE_DIR)/ 2>/dev/null || true
	@cp -a $(PY_LIB_DIR)/rumi*.dll $(STAGE_DIR)/ 2>/dev/null || true
	@ls -1 $(STAGE_DIR)

python: lib
	@$(PYTHON) -c 'import numpy, cffi' 2>/dev/null \
	  || { echo "missing runtime deps, install: numpy cffi"; exit 1; }
	$(PYTHON) -m pip install -e $(PY_DIR) -q
	$(PYTHON) -c "import rumi; print('rumi', rumi.__version__)"

# geozl is what compresses, so the write path needs it installed even though
# rumi never imports it.
test: python
	@$(PYTHON) -c 'import pytest' 2>/dev/null || { echo "pytest not installed"; exit 1; }
	@$(PYTHON) -c 'import geozl' 2>/dev/null \
	  || echo "note: geozl not installed, write tests will skip"
	@$(PYTHON) -m pytest -q $(PY_DIR); \
	  rc=$$?; if [ $$rc -eq 5 ]; then echo "no tests collected"; \
	  elif [ $$rc -ne 0 ]; then exit $$rc; fi

# R binding (terra, system GDAL). Skips cleanly until bindings/r exists.
r:
	@if [ ! -d $(R_DIR) ]; then \
	  echo "no R binding yet ($(R_DIR) absent); skipping"; \
	else \
	  command -v R >/dev/null || { echo "missing R"; exit 1; }; \
	  ( cd $(R_DIR) && Rscript -e 'if (requireNamespace("roxygen2", quietly=TRUE)) roxygen2::roxygenise()' ); \
	  R CMD INSTALL $(R_DIR); \
	  R CMD build $(R_DIR); \
	fi

# Python and CMake read VERSION at build time, so only a hardcoded manifest
# needs rewriting. That is R's DESCRIPTION, written here when bindings/r exists.
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

install: build
	cmake --install $(BUILD_DIR) --prefix $(PREFIX)

clean:
	rm -rf $(BUILD_DIR) $(STAGE_DIR)
	rm -rf $(PY_DIR)/build $(PY_DIR)/*.egg-info .pytest_cache
	rm -f $(PY_LIB_DIR)/librumi* $(PY_LIB_DIR)/rumi*.dll
	find . -type d -name __pycache__ -prune -exec rm -rf {} +
	find . -type f -name '*.pyc' -delete

help:
	@echo "make            build, stage the lib, editable install, smoke load"
	@echo "make build      build librumi and the GDAL plugin only"
	@echo "make lib        build and stage the shared lib next to the binding (CI entry)"
	@echo "make test       build, install, then pytest"
	@echo "make r          build the R binding (skips until $(R_DIR) exists)"
	@echo "make sync       validate VERSION; write R DESCRIPTION if present"
	@echo "make install    cmake --install into PREFIX ($(PREFIX))"
	@echo "make clean      remove all build output, caches and generated files"
	@echo "make submodules fetch or update geozl (and OpenZL under it)"
	@echo ""
	@echo "vars  BUILD=Debug  PYTHON=python3.12  GEN='Unix Makefiles'  PREFIX=/opt"
	@echo "      PLUGIN=OFF (skip the GDAL auto-load plugin, lib only)"
	@echo "      STAGE_DIR=out  CMAKE_FLAGS=-DCMAKE_PREFIX_PATH=/opt/gdal"
	@echo ""
	@echo "GDAL 3.8 or newer is a system dependency. To use the plugin without"
	@echo "installing, point GDAL at the build tree:"
	@echo "      GDAL_DRIVER_PATH=$(BUILD_DIR) gdalinfo -oo RUMI_HEADER=... file.rumi"