# make            build, stage the lib, editable install, smoke load
# make build      build librumi only
# make lib        build and stage the shared lib next to the binding (CI entry)
# make test       run the Python and JavaScript suites
# make js-install install the locked JavaScript dependencies
# make js-test    run the JavaScript reader suite
# make playground serve the JavaScript MapLibre playground
# make lint       run Ruff and mypy on the Python binding
# make ctest      build and run the C++ component tests
# make r          build the R binding
# make r-vendor   copy native sources into the R source package
# make r-vendor-check verify that the copied R sources are current
# make r-check    build and check the standalone R source package
# make julia      load the Julia binding
# make bindings-test test the R and Julia readers
# make bindings-fixtures refresh the small package test fixtures
# make sync       validate VERSION; update binding manifests
# make install    cmake --install into PREFIX (/usr/local)
# make docs       render SPEC.md into a deployable copy of docs/
# make clean      remove all build output, caches and generated files
# make submodules fetch or update geozl, Karu, and their nested dependencies

# `make help` lists targets and configurable variables.

PYTHON ?= python
PREFIX ?= /usr/local
BUILD_TYPE ?= Release
GEN    ?= Ninja

CORE      := core
BUILD_DIR := core/build
GEOZL     := extern/geozl
KARU      := extern/karu
PY_DIR    := bindings/python
JS_DIR    := bindings/javascript
R_DIR     := bindings/r
JL_DIR    := bindings/julia
PY_LIB_DIR := $(PY_DIR)/rumi/_lib
STAGE_DIR ?= staged
FUZZ_TIME ?= 60
FUZZ_JOBS ?= 0
FUZZ_OUT  := fuzz/out
FUZZ_CORPUS := fuzz/corpus
FUZZ_SEEDS := fuzz/replay
FUZZ_TARGETS := header index pattern read
UNAME     := $(shell uname -s)
VERSION   := $(shell tr -d '[:space:]' < VERSION)

ifeq ($(UNAME),Darwin)
  LIBRUMI := librumi.dylib
else ifeq ($(OS),Windows_NT)
  LIBRUMI := rumi.dll
else
  LIBRUMI := librumi.so
endif

# The Python binding loads the shared library at runtime.
CMAKE_FLAGS ?=
CMAKE_OPTS  := -G $(GEN) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) \
               -DRUMI_BUILD_SHARED_LIB=ON $(CMAKE_FLAGS)

.PHONY: all build configure lib stage-lib python test js-install js-test playground lint ctest docs \
        r r-vendor r-vendor-check r-check julia bindings-test bindings-fixtures sync \
        fuzz-build fuzz-seed fuzz fuzz-report fuzz-check fuzz-replay clean-fuzz \
        install submodules clean help

all: python

# Initialize native submodules on first build.
$(GEOZL)/core/CMakeLists.txt $(KARU)/CMakeLists.txt:
	git submodule update --init --recursive

submodules:
	git submodule update --init --recursive

$(BUILD_DIR)/CMakeCache.txt: VERSION $(GEOZL)/core/CMakeLists.txt $(KARU)/CMakeLists.txt
	cmake -S $(CORE) -B $(BUILD_DIR) $(CMAKE_OPTS)

configure: $(GEOZL)/core/CMakeLists.txt $(KARU)/CMakeLists.txt
	cmake -S $(CORE) -B $(BUILD_DIR) $(CMAKE_OPTS)

build: $(BUILD_DIR)/CMakeCache.txt
	cmake --build $(BUILD_DIR)

# Stage one shared-library file beside the CFFI binding.
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
	$(PYTHON) tools/check_editable.py

# Python tests use geozl to create compressed frame payloads.
test: python js-test
	@$(PYTHON) -c 'import pytest' 2>/dev/null || { echo "pytest not installed"; exit 1; }
	@$(PYTHON) -c 'import geozl' 2>/dev/null \
	  || echo "note: geozl not installed, write tests will skip"
	@$(PYTHON) -m pytest -q $(PY_DIR); \
	  rc=$$?; if [ $$rc -eq 5 ]; then echo "no tests collected"; \
	  elif [ $$rc -ne 0 ]; then exit $$rc; fi

js-install:
	@command -v node >/dev/null || { echo "missing Node.js 24 or newer"; exit 1; }
	@node -e 'const major = Number(process.versions.node.split(".")[0]); process.exit(major >= 24 ? 0 : 1)' \
	  || { echo "Node.js 24 or newer is required"; exit 1; }
	cd $(JS_DIR) && npm ci --ignore-scripts --no-audit --no-fund

js-test:
	@command -v node >/dev/null || { echo "missing Node.js 24 or newer"; exit 1; }
	@node -e 'const major = Number(process.versions.node.split(".")[0]); process.exit(major >= 24 ? 0 : 1)' \
	  || { echo "Node.js 24 or newer is required"; exit 1; }
	cd $(JS_DIR) && npm test

playground:
	cd $(JS_DIR) && npm run playground

lint:
	@$(PYTHON) -c 'import ruff' 2>/dev/null \
	  || { echo "ruff not installed"; exit 1; }
	@$(PYTHON) -c 'import mypy' 2>/dev/null \
	  || { echo "mypy not installed"; exit 1; }
	$(PYTHON) -m ruff check --config $(PY_DIR)/pyproject.toml \
	  $(PY_DIR) tools/check_editable.py
	$(PYTHON) -m mypy --config-file $(PY_DIR)/pyproject.toml $(PY_DIR)/rumi

# Component tests use an independent build directory.
ctest: $(GEOZL)/core/CMakeLists.txt
	cmake -S $(CORE) -B $(BUILD_DIR)-tests -G $(GEN) \
	  -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) -DRUMI_BUILD_TESTS=ON $(CMAKE_FLAGS)
	cmake --build $(BUILD_DIR)-tests --target rumi_tests
	$(BUILD_DIR)-tests/rumi_tests

# Prefer Homebrew LLVM on macOS because Apple clang omits libFuzzer.
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
	cmake --build core/build-fuzz --target $(addsuffix _fuzzer,$(addprefix rumi_,$(FUZZ_TARGETS)))

fuzz-seed:
	@for t in $(FUZZ_TARGETS); do \
	  mkdir -p $(FUZZ_CORPUS)/$$t; \
	  cp -a $(FUZZ_SEEDS)/$$t/. $(FUZZ_CORPUS)/$$t/; \
	done

# libc++ container annotations produce false positives while loading the corpus.
# Above 1 GiB, OpenZL's malloc returns NULL instead of aborting the fuzzer.
# This is a fuzz-only guard; production decoding has no equivalent budget.
FUZZ_ASAN := allocator_may_return_null=1:detect_container_overflow=0:max_allocation_size_mb=1024

fuzz: fuzz-build fuzz-seed
	@mkdir -p $(FUZZ_OUT)
	@for t in $(FUZZ_TARGETS); do \
	  corpus=$(abspath $(FUZZ_CORPUS))/$$t; \
	  echo "$$t fuzzer, $(FUZZ_TIME)s"; \
	  (cd $(FUZZ_OUT) && ASAN_OPTIONS=$(FUZZ_ASAN) \
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
	  (cd $(FUZZ_OUT) && ASAN_OPTIONS=$(FUZZ_ASAN) \
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

# Build the static site from docs/ into the disposable _site directory.
docs:
	$(PYTHON) tools/build_docs.py --output _site --clean

r-vendor: $(GEOZL)/core/CMakeLists.txt $(KARU)/CMakeLists.txt
	$(PYTHON) tools/sync_r_vendor.py

r-vendor-check: $(GEOZL)/core/CMakeLists.txt $(KARU)/CMakeLists.txt
	$(PYTHON) tools/sync_r_vendor.py --check

bindings-fixtures: python
	$(PYTHON) tools/make_binding_fixture.py \
		$(R_DIR)/tests/testthat/fixtures $(JL_DIR)/test/fixtures

r: r-vendor bindings-fixtures
	@command -v R >/dev/null || { echo "missing R"; exit 1; }
	cd $(R_DIR) && Rscript -e 'if (requireNamespace("roxygen2", quietly=TRUE)) roxygen2::roxygenise()'
	R CMD build $(R_DIR)
	R CMD INSTALL rumi_$(VERSION).tar.gz

r-check: r-vendor-check bindings-fixtures
	@command -v R >/dev/null || { echo "missing R"; exit 1; }
	cd $(R_DIR) && Rscript -e 'if (requireNamespace("roxygen2", quietly=TRUE)) roxygen2::roxygenise()'
	R CMD build $(R_DIR)
	_R_CHECK_CRAN_INCOMING_REMOTE_=false R CMD check --no-manual rumi_$(VERSION).tar.gz
	@sh tools/r_check_status.sh rumi.Rcheck/00check.log

julia: build
	@command -v julia >/dev/null || { echo "missing Julia"; exit 1; }
	RUMI_LIB=$(abspath $(BUILD_DIR)/$(LIBRUMI)) \
		julia --startup-file=no --project=$(JL_DIR) -e 'using Rumi'

bindings-test: python bindings-fixtures
	@command -v R >/dev/null || { echo "missing R"; exit 1; }
	@command -v julia >/dev/null || { echo "missing Julia"; exit 1; }
	R CMD INSTALL $(R_DIR)
	Rscript -e 'testthat::test_dir("$(R_DIR)/tests/testthat")'
	RUMI_LIB=$(abspath $(BUILD_DIR)/$(LIBRUMI)) \
		julia --startup-file=no --project=$(JL_DIR) $(JL_DIR)/test/runtests.jl

# Python and CMake read VERSION directly; update binding manifests when present.
sync:
	@printf '%s' "$(VERSION)" | grep -qE '^[0-9]+\.[0-9]+\.[0-9]+(-[0-9A-Za-z.]+)?$$' \
	  || { echo "VERSION '$(VERSION)' is not X.Y.Z[-prerelease]"; exit 1; }
	@set -e; if [ -f $(R_DIR)/DESCRIPTION ]; then \
	  r_version=$$($(PYTHON) tools/check_release.py --print-r-version); \
	  sed -i.bak -E "s/^Version:.*/Version: $$r_version/" $(R_DIR)/DESCRIPTION; \
	  rm -f $(R_DIR)/DESCRIPTION.bak; \
	  v=$$(grep -E '^Version:' $(R_DIR)/DESCRIPTION | sed 's/Version:[[:space:]]*//'); \
	  [ "$$v" = "$$r_version" ] || { echo "sync check: DESCRIPTION=$$v != $$r_version"; exit 1; }; \
	  echo "sync OK $(VERSION) (python dynamic, R DESCRIPTION written)"; \
	else \
	  echo "sync OK $(VERSION) (python dynamic, no R binding yet)"; \
	fi
	@set -e; if [ -f $(JL_DIR)/Project.toml ]; then \
	  sed -i.bak -E 's/^version = .*/version = "$(VERSION)"/' $(JL_DIR)/Project.toml; \
	  rm -f $(JL_DIR)/Project.toml.bak; \
	  v=$$(grep -E '^version = ' $(JL_DIR)/Project.toml | sed -E 's/version = "([^"]+)"/\1/'); \
	  [ "$$v" = "$(VERSION)" ] || { echo "sync check: Project.toml=$$v != $(VERSION)"; exit 1; }; \
	  echo "sync OK $(VERSION) (Julia Project.toml written)"; \
	fi
	@set -e; if [ -f $(JS_DIR)/package.json ]; then \
	  cd $(JS_DIR); \
	  npm version "$(VERSION)" --no-git-tag-version --allow-same-version --ignore-scripts >/dev/null; \
	  v=$$(node -p 'require("./package.json").version'); \
	  [ "$$v" = "$(VERSION)" ] || { echo "sync check: package.json=$$v != $(VERSION)"; exit 1; }; \
	  echo "sync OK $(VERSION) (JavaScript package.json written)"; \
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
	@echo "make test       run the Python and JavaScript suites"
	@echo "make js-install install the locked JavaScript dependencies"
	@echo "make js-test    run the JavaScript reader suite"
	@echo "make playground serve the JavaScript MapLibre playground"
	@echo "make lint       run Ruff and mypy on the Python binding"
	@echo "make ctest      build and run the C++ component tests"
	@echo "make fuzz-check build and run the libFuzzer harnesses"
	@echo "make fuzz-replay replay the versioned and cached fuzz inputs"
	@echo "make docs       render SPEC.md into a deployable copy of docs/"
	@echo "make r          build and install the standalone R source package"
	@echo "make r-vendor   copy native sources into the R source package"
	@echo "make r-vendor-check verify that the copied R sources are current"
	@echo "make r-check    build and check the standalone R source package"
	@echo "make julia      load the Julia binding against the local library"
	@echo "make bindings-test test the R and Julia readers on one shared fixture"
	@echo "make bindings-fixtures refresh the R and Julia package fixtures"
	@echo "make sync       write package versions from VERSION"
	@echo "make install    cmake --install into PREFIX ($(PREFIX))"
	@echo "make clean      remove all build output, caches and generated files"
	@echo "make submodules fetch or update GeoZL, Karu, and nested dependencies"
	@echo ""
	@echo "vars  BUILD_TYPE=Debug  PYTHON=python3.12  GEN='Unix Makefiles'  PREFIX=/opt"
	@echo "      STAGE_DIR=out  CMAKE_FLAGS=-DCMAKE_BUILD_TYPE=Debug"
	@echo ""
