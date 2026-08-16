# make            build, stage the lib, editable install, smoke load
# make build      build librumi only
# make lib        build and stage the shared lib next to the binding (CI entry)
# make test       build, install, then pytest
# make ctest      build and run the C++ component tests
# make r          build the R binding (skips until bindings/r exists)
# make sync       validate VERSION; write R DESCRIPTION if present
# make install    cmake --install into PREFIX (/usr/local)
# make docs       render SPEC.md into a deployable copy of docs/
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
FUZZ_TIME ?= 60
FUZZ_JOBS ?= 0
FUZZ_OUT  := fuzz/out
FUZZ_CORPUS := fuzz/corpus
FUZZ_SEEDS := fuzz/replay
FUZZ_TARGETS := header index
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

.PHONY: all build configure lib stage-lib python test ctest docs r sync \
        fuzz-build fuzz-seed fuzz fuzz-report fuzz-check fuzz-replay clean-fuzz \
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
	@f=$(BUILD_DIR)/$(LIBRUMI); \
	  [ -f "$$f" ] || { echo "no $$f"; exit 1; }; \
	  cp -L "$$f" $(PY_LIB_DIR)/$(LIBRUMI); \
	  echo "staged $(LIBRUMI) into $(PY_LIB_DIR)"

# Copy the lib and legal notices to STAGE_DIR for release artifacts.
stage-lib: lib
	@mkdir -p $(STAGE_DIR)/licenses
	@cp -a $(PY_LIB_DIR)/librumi* $(STAGE_DIR)/ 2>/dev/null || true
	@cp -a $(PY_LIB_DIR)/rumi*.dll $(STAGE_DIR)/ 2>/dev/null || true
	@cp LICENSE NOTICE $(STAGE_DIR)/
	@cp licenses/LICENSE.* $(STAGE_DIR)/licenses/
	@find $(STAGE_DIR) -maxdepth 2 -type f -print

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

# Apple Command Line Tools clang has no libFuzzer runtime. Prefer Homebrew LLVM
# on macOS when it exists; Linux CI uses the system clang.
ifeq ($(UNAME),Darwin)
  BREW_LLVM := $(shell brew --prefix llvm 2>/dev/null)/bin/clang
  CLANG ?= $(if $(wildcard $(BREW_LLVM)),$(BREW_LLVM),clang)
else
  CLANG ?= clang
endif
CLANGXX ?= $(CLANG)++

fuzz-build: $(GEOZL)/core/CMakeLists.txt
	cmake -S $(CORE) -B core/build-fuzz -G $(GEN) \
	  -DCMAKE_BUILD_TYPE=RelWithDebInfo -DRUMI_BUILD_FUZZERS=ON \
	  -DRUMI_SANITIZE=address,undefined \
	  -DCMAKE_C_COMPILER=$(CLANG) -DCMAKE_CXX_COMPILER=$(CLANGXX)
	cmake --build core/build-fuzz --target rumi_header_fuzzer rumi_index_fuzzer

fuzz-seed:
	@for t in $(FUZZ_TARGETS); do \
	  mkdir -p $(FUZZ_CORPUS)/$$t; \
	  cp -a $(FUZZ_SEEDS)/$$t/. $(FUZZ_CORPUS)/$$t/; \
	done

fuzz: fuzz-build fuzz-seed
	@mkdir -p $(FUZZ_OUT)
	@for t in $(FUZZ_TARGETS); do \
	  corpus=$(abspath $(FUZZ_CORPUS))/$$t; \
	  echo "$$t fuzzer, $(FUZZ_TIME)s"; \
	  (cd $(FUZZ_OUT) && ASAN_OPTIONS=allocator_may_return_null=1 \
	    $(abspath core/build-fuzz)/rumi_$${t}_fuzzer $$corpus \
	    -max_total_time=$(FUZZ_TIME) -max_len=65536 -jobs=$(FUZZ_JOBS) \
	    -artifact_prefix=$(abspath $(FUZZ_OUT))/ > $$t.log 2>&1) || true; \
	done

fuzz-report:
	@{ \
	  echo "rumi fuzz report"; \
	  echo "$(FUZZ_TIME)s per target, jobs $(FUZZ_JOBS)"; \
	  for t in $(FUZZ_TARGETS); do \
	    echo; echo "== $$t =="; \
	    grep -hE 'INITED|DONE|Loaded . modules' \
	      $(FUZZ_OUT)/$$t.log 2>/dev/null | head -4 || true; \
	    grep -hB2 -A12 -E 'runtime error|ERROR:|SUMMARY:' \
	      $(FUZZ_OUT)/$$t.log 2>/dev/null | head -30 || true; \
	  done; \
	  echo; echo "== findings =="; \
	  found=$$(ls -1 $(FUZZ_OUT) 2>/dev/null | grep -E '^(crash|oom|leak|timeout)-'); \
	  if [ -z "$$found" ]; then echo "none"; else echo "$$found"; fi; \
	} > $(FUZZ_OUT)/report.txt
	@cat $(FUZZ_OUT)/report.txt

fuzz-check: fuzz fuzz-report
	@found=$$(ls -1 $(FUZZ_OUT) 2>/dev/null | \
	  grep -E '^(crash|oom|leak|timeout)-' || true); \
	[ -z "$$found" ] || { echo "fuzzing found:"; echo "$$found"; exit 1; }; \
	echo "no findings in $(FUZZ_TIME)s per target"

fuzz-replay: fuzz-build
	@mkdir -p $(FUZZ_OUT)
	@failed=""; \
	for t in $(FUZZ_TARGETS); do \
	  seeds=$(abspath $(FUZZ_SEEDS))/$$t; \
	  corpus=$(abspath $(FUZZ_CORPUS))/$$t; \
	  dirs=$$seeds; \
	  test -n "$$(ls -A $$corpus 2>/dev/null)" && dirs="$$dirs $$corpus"; \
	  echo "$$t replay"; \
	  (cd $(FUZZ_OUT) && ASAN_OPTIONS=allocator_may_return_null=1 \
	    $(abspath core/build-fuzz)/rumi_$${t}_fuzzer $$dirs \
	    -runs=0 -max_len=65536 \
	    -artifact_prefix=$(abspath $(FUZZ_OUT))/ > $$t.log 2>&1) \
	    || { failed="$$failed $$t"; tail -30 $(FUZZ_OUT)/$$t.log; }; \
	done; \
	[ -z "$$failed" ] || { echo "failed:$$failed"; exit 1; }; \
	echo "all replay inputs passed"

clean-fuzz:
	rm -rf $(FUZZ_OUT) $(FUZZ_CORPUS) core/build-fuzz
	rm -f crash-* leak-* timeout-* oom-* fuzz-*.log

# Static GitHub Pages website. docs/ is the source; _site is only the staged
# artifact. Install the one pinned dependency from docs/requirements.txt first.
docs:
	$(PYTHON) tools/build_docs.py --output _site --clean

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

clean: clean-fuzz
	rm -rf $(BUILD_DIR) $(BUILD_DIR)-tests $(STAGE_DIR)
	rm -rf $(PY_DIR)/build $(PY_DIR)/*.egg-info .pytest_cache _site
	rm -f $(PY_LIB_DIR)/librumi* $(PY_LIB_DIR)/rumi*.dll
	find . -type d -name __pycache__ -prune -exec rm -rf {} +
	find . -type f -name '*.pyc' -delete

help:
	@echo "make            build, stage the lib, editable install, smoke load"
	@echo "make build      build librumi only"
	@echo "make lib        build and stage the shared lib next to the binding (CI entry)"
	@echo "make test       build, install, then pytest"
	@echo "make ctest      build and run the C++ component tests"
	@echo "make fuzz-check build and run the libFuzzer harnesses"
	@echo "make fuzz-replay replay the versioned and cached fuzz inputs"
	@echo "make docs       render SPEC.md into a deployable copy of docs/"
	@echo "make r          build the R binding (skips until $(R_DIR) exists)"
	@echo "make sync       validate VERSION; write R DESCRIPTION if present"
	@echo "make install    cmake --install into PREFIX ($(PREFIX))"
	@echo "make clean      remove all build output, caches and generated files"
	@echo "make submodules fetch or update geozl (and OpenZL under it)"
	@echo ""
	@echo "vars  BUILD_TYPE=Debug  PYTHON=python3.12  GEN='Unix Makefiles'  PREFIX=/opt"
	@echo "      STAGE_DIR=out  CMAKE_FLAGS=-DCMAKE_BUILD_TYPE=Debug"
	@echo ""
