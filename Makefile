CXX ?= g++
# Vendor httplib.h is a system include so upstream warnings are outside -Werror.
CPPFLAGS += -isystem vendor
CPPFLAGS += $(shell pkg-config --cflags libpq 2>/dev/null)
LDLIBS += $(shell pkg-config --libs libpq 2>/dev/null)
ifeq ($(strip $(LDLIBS)),)
  LDLIBS += -lpq
endif
LDLIBS += -pthread

# Fly binary: optimized and stripped. Sanitizers stay off this link (they slow cold start).
PROD_CXXFLAGS := -std=c++17 -O2 -Wall -Wextra -Werror -pthread -g0 -s
# Functional tests only. -Wno-maybe-uninitialized silences a libstdc++ regex
# false positive that appears when sanitizers instrument httplib route patterns.
TEST_CXXFLAGS := -std=c++17 -O2 -g -Wall -Wextra -Werror -Wno-maybe-uninitialized -pthread -fno-omit-frame-pointer -fsanitize=address,undefined

CPPCHECK ?= cppcheck
OSV_SCANNER ?= osv-scanner
GITLEAKS ?= gitleaks
CLANG_FORMAT ?= clang-format
FIRST_PARTY_CXX := src/main.cpp src/perf_test.cpp src/carolina.h

need = command -v $(1) >/dev/null 2>&1 || { echo "missing $(1); install it to run this check" >&2; exit 1; }

.PHONY: all clean test sast vuln secrets fmt fmt-check check precommit ci hooks

all: api

api: src/main.cpp src/carolina.h vendor/httplib.h
	$(CXX) $(PROD_CXXFLAGS) $(CPPFLAGS) -o $@ src/main.cpp $(LDLIBS)

perf_test: src/main.cpp src/perf_test.cpp src/carolina.h vendor/httplib.h
	$(CXX) $(TEST_CXXFLAGS) -DCAROLINA_TEST $(CPPFLAGS) -Isrc -o $@ src/main.cpp src/perf_test.cpp $(LDLIBS)

test: api perf_test
	./perf_test

# First-party src/ only. Findings in vendor/httplib.h are not this tree's to fix.
sast:
	@$(call need,$(CPPCHECK))
	$(CPPCHECK) --error-exitcode=1 --std=c++17 \
		--enable=warning,performance,portability \
		--inline-suppr --suppress=missingIncludeSystem \
		--suppress=unmatchedSuppression --suppress='*:vendor/*' \
		-I vendor -I src src

# No lockfile; recursive source scan with git-root C/C++ (vendored / determineversion).
vuln:
	@$(call need,$(OSV_SCANNER))
	$(OSV_SCANNER) scan source -r --include-git-root .

secrets:
	@$(call need,$(GITLEAKS))
	gitleaks detect --source . --no-banner --verbose

fmt:
	@$(call need,$(CLANG_FORMAT))
	$(CLANG_FORMAT) -i $(FIRST_PARTY_CXX)

fmt-check:
	@$(call need,$(CLANG_FORMAT))
	$(CLANG_FORMAT) --dry-run --Werror $(FIRST_PARTY_CXX)

check: test sast vuln secrets fmt-check
precommit: check
ci: check

hooks:
	@command -v pre-commit >/dev/null 2>&1 && pre-commit install || true
	git config core.hooksPath .githooks

clean:
	rm -f api perf_test
