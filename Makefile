# make            build, stage the lib, editable install, smoke load
# make build      build librumi only
# make lib        build and stage the shared lib next to the binding (CI entry)
# make test       build, install, then pytest
# make ctest      build and run the C++ component tests
# make r          build the R binding (skips until bindings/r exists)
# make sync       validate VERSION; write R DESCRIPTION if present
# make install    cmake --install into PREFIX (/usr/local)
# make clean      remove all build output, caches and generated files
# make submodules fetch or update geozl (and OpenZL under it)

# rumi build. `make help` lists the targets and the variables.
# Vendors geozl as a submodule, fetched on first build. Nothing else is needed:
# rumi links no system library beyond libc and the C++ runtime.

PYTHON ?= python
PREFIX ?= /usr/local
BUILD_TYPE ?= Release
GEN    ?= Ninja

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

# RUMI_BUILD_SHARED_LIB is on because the Python binding dlopens that lib.
CMAKE_FLAGS ?=
CMAKE_OPTS  := -G $(GEN) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
               -DRUMI_BUILD_SHARED_LIB=ON $(CMAKE_FLAGS)

.PHONY: all build configure lib stage-lib python test ctest r sync \
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

# Stage the shared lib next to the binding, cffi loads it from there. One file,
# under the plain soname: a wheel turns the version symlinks into full copies,
# and nothing here resolves a soname anyway, cffi dlopens the path it finds.
lib: build
	@mkdir -p $(PY_LIB_DIR)
	@rm -f $(PY_LIB_DIR)/librumi* $(PY_LIB_DIR)/rumi*.dll
	@f=$$(find $(BUILD_DIR) \( -name 'librumi*.dylib' \
	        -o -name 'librumi*.so.*' -o -name 'rumi*.dll' \) \
	      -type f | head -1); \
	  [ -n "$$f" ] || { echo "no $(LIBRUMI) under $(BUILD_DIR)"; exit 1; }; \
	  cp "$$f" $(PY_LIB_DIR)/$(LIBRUMI); \
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

# Its own build dir, so it never disturbs the lib staged for the binding.
ctest: $(GEOZL)/core/CMakeLists.txt
	cmake -S $(CORE) -B $(BUILD_DIR)-tests -G $(GEN) \
	  -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DRUMI_BUILD_TESTS=ON $(CMAKE_FLAGS)
	cmake --build $(BUILD_DIR)-tests --target rumi_tests
	$(BUILD_DIR)-tests/rumi_tests

# R binding. Skips cleanly until bindings/r exists.
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
	rm -rf $(BUILD_DIR) $(BUILD_DIR)-tests $(STAGE_DIR)
	rm -rf $(PY_DIR)/build $(PY_DIR)/*.egg-info .pytest_cache
	rm -f $(PY_LIB_DIR)/librumi* $(PY_LIB_DIR)/rumi*.dll
	find . -type d -name __pycache__ -prune -exec rm -rf {} +
	find . -type f -name '*.pyc' -delete

help:
	@echo "make            build, stage the lib, editable install, smoke load"
	@echo "make build      build librumi only"
	@echo "make lib        build and stage the shared lib next to the binding (CI entry)"
	@echo "make test       build, install, then pytest"
	@echo "make r          build the R binding (skips until $(R_DIR) exists)"
	@echo "make sync       validate VERSION; write R DESCRIPTION if present"
	@echo "make install    cmake --install into PREFIX ($(PREFIX))"
	@echo "make clean      remove all build output, caches and generated files"
	@echo "make submodules fetch or update geozl (and OpenZL under it)"
	@echo ""
	@echo "vars  BUILD_TYPE=Debug  PYTHON=python3.12  GEN='Unix Makefiles'  PREFIX=/opt"
	@echo "      STAGE_DIR=out  CMAKE_FLAGS=-DCMAKE_BUILD_TYPE=Debug"
	@echo ""
