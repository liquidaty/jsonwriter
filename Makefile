# Makefile for use with GNU make

THIS_MAKEFILE_DIR:=$(shell dirname $(realpath $(lastword $(MAKEFILE_LIST))))
THIS_DIR:=$(shell basename "${THIS_MAKEFILE_DIR}")
THIS_MAKEFILE:=$(lastword $(MAKEFILE_LIST))

.POSIX:
.SUFFIXES:
.SUFFIXES: .o .c .a

CONFIGFILE ?= config.mk
$(info Using config file ${CONFIGFILE})
include ${CONFIGFILE}

CC ?= cc
AWK ?= awk
AR ?= ar
RANLIB ?= ranlib
SED ?= sed

WIN=
DEBUG=0
ifeq ($(WIN),)
  WIN=0
  ifneq ($(findstring w64,$(CC)),) # e.g. mingw64
    WIN=1
  endif
endif

CFLAGS+=${CFLAG_O} ${CFLAGS_OPT}
CFLAGS+=${CFLAGS_AUTO}
CFLAGS+=-I.

ifeq ($(VERBOSE),1)
  CFLAGS+= ${CFLAGS_VECTORIZE_OPTIMIZED} ${CFLAGS_VECTORIZE_MISSED} ${CFLAGS_VECTORIZE_ALL}
endif

VERSION= $(shell (git describe --always --dirty --tags 2>/dev/null || echo "v0.0.0-jsonwriter") | sed 's/^v//')

ifneq ($(findstring emcc,$(CC)),) # emcc
  NO_THREADING=1
endif

ifeq ($(NO_THREADING),1)
  CFLAGS+= -DNO_THREADING
endif

ASAN ?= 0
ifeq ($(ASAN),1)
  SANITIZE_FLAGS= -fsanitize=address,undefined -fno-omit-frame-pointer
  CFLAGS+= ${SANITIZE_FLAGS} -g -O1 -UNDEBUG
  LDFLAGS+= ${SANITIZE_FLAGS}
  DBG_SUBDIR=san
else ifeq ($(DEBUG),1)
  CFLAGS+= ${CFLAGS_DEBUG}
  DBG_SUBDIR=dbg
else
  CFLAGS+= -DNDEBUG -O3  ${CFLAGS_LTO}
  DBG_SUBDIR=rel
endif

ifeq ($(WIN),0)
  BUILD_SUBDIR=$(shell uname)/${DBG_SUBDIR}
  WHICH=which
  EXE=
  CFLAGS+= -fPIC
else
  BUILD_SUBDIR=win/${DBG_SUBDIR}
  WHICH=where
  EXE=.exe
  CFLAGS+= -fpie
  CFLAGS+= -D__USE_MINGW_ANSI_STDIO -D_ISOC99_SOURCE -Wl,--strip-all
endif

CFLAGS+= -std=gnu11 -Wno-gnu-statement-expression -Wshadow -Wall -Wextra -Wno-missing-braces -pedantic -D_GNU_SOURCE

CFLAGS+= ${JSONWRITER_OPTIONAL_CFLAGS}

CCBN=$(shell basename ${CC})
THIS_LIB_BASE=$(shell cd .. && pwd)
INCLUDE_DIR=${THIS_LIB_BASE}/include
BUILD_DIR=${THIS_LIB_BASE}/build/${BUILD_SUBDIR}/${CCBN}

NO_UTF8_CHECK=1

LIB_SUFFIX?=
JSONWRITER_OBJ=${BUILD_DIR}/objs/jsonwriter.o
LIBJSONWRITER_A=libjsonwriter${LIB_SUFFIX}.a
LIBJSONWRITER=${BUILD_DIR}/lib/${LIBJSONWRITER_A}
LIBJSONWRITER_INSTALL=${LIBDIR}/${LIBJSONWRITER_A}

JSONWRITER_OBJ_OPTS=
ifeq ($(NO_UTF8_CHECK),1)
  JSONWRITER_OBJ_OPTS+= -DNO_UTF8_CHECK
endif

help:
	@echo "Make options:"
	@echo "  `basename ${MAKE}` build|install|uninstall|clean"
	@echo
	@echo "Test/QA targets:"
	@echo "  `basename ${MAKE}` test|check|test-leaks|strict|fuzz|fuzz-standalone|print-builddir"
	@echo "  `basename ${MAKE}` df-help   # dockerized libFuzzer fuzzing (runs on macOS / Apple Silicon)"
	@echo
	@echo "Optional make variables:"
	@echo "  [CONFIGFILE=config.mk] [NO_UTF8_CHECK=1] [ASAN=1] [DEBUG=1] [VERBOSE=1] [LIBDIR=${LIBDIR}] [INCLUDEDIR=${INCLUDEDIR}] [LIB_SUFFIX=]"
	@echo

build: ${LIBJSONWRITER}

${LIBJSONWRITER}: ${JSONWRITER_OBJ}
	@mkdir -p `dirname "$@"`
	@rm -f $@
	$(AR) rcv $@ $?
	$(RANLIB) $@
	$(AR) -t $@ # check it is there
	@echo Built $@

install: ${LIBJSONWRITER_INSTALL}
	@mkdir -p  $(INCLUDEDIR)
	@cp -pR jsonwriter.h $(INCLUDEDIR)
	@echo "include files copied to $(INCLUDEDIR)"

${LIBJSONWRITER_INSTALL}: ${LIBJSONWRITER}
	@mkdir -p `dirname "$@"`
	@cp -p ${LIBJSONWRITER} "$@"
	@echo "libjsonwriter installed to $@"

uninstall:
	@rm -rf ${INCLUDEDIR}/jsonwriter*
	 rm  -f ${LIBDIR}/libjsonwriter*

clean:
	rm -rf ${BUILD_DIR}/objs ${BUILD_DIR}/bin ${LIBJSONWRITER}

.PHONY: build install uninstall clean  ${LIBJSONWRITER_INSTALL}

${BUILD_DIR}/objs/jsonwriter.o: jsonwriter.c jsonwriter.h utils.c json_numeric.c
	@mkdir -p `dirname "$@"`
	${CC} ${CFLAGS} -DINCLUDE_UTILS -DJSONWRITER_VERSION=\"${VERSION}\" -I${INCLUDE_DIR} ${JSONWRITER_OBJ_OPTS} -o $@ -c $<

# ----------------------------------------------------------------- tests / QA
# Unit tests, leak check, a strict-warnings lane, and fuzzing. All binaries land
# in the per-target build dir alongside the library -- nothing is written to the
# source tree. Run the suite under AddressSanitizer + UBSan with: make test ASAN=1
TESTS_DIR= tests
TEST_SRC= ${TESTS_DIR}/test.c
FUZZ_SRC= ${TESTS_DIR}/fuzz.c
FUZZ_CORPUS= ${TESTS_DIR}/corpus
TEST_BIN= ${BUILD_DIR}/bin/test${EXE}
FUZZ_BIN= ${BUILD_DIR}/bin/fuzz${EXE}
FUZZ_STANDALONE_BIN= ${BUILD_DIR}/bin/fuzz-standalone${EXE}

# libFuzzer needs an LLVM clang with the fuzzer runtime (Apple clang lacks it);
# override with FUZZ_CC=... The standalone driver below builds with any compiler.
FUZZ_CC?= clang

# Strictest warning set the library compiles clean under. -Wconversion is left
# out deliberately: the existing code is not written to it and adding it would
# force a rewrite rather than catch a defect.
STRICT_CFLAGS= -std=c11 -pedantic -Wall -Wextra -Werror -Wshadow -Wstrict-prototypes -Wvla

# Single compile+link step so the sanitizer flags carried in CFLAGS (ASAN=1)
# reach the link line too. The library is rebuilt first via its prerequisite.
${TEST_BIN}: ${TEST_SRC} jsonwriter.h ${LIBJSONWRITER}
	@mkdir -p `dirname "$@"`
	${CC} ${CFLAGS} -I. -o $@ ${TEST_SRC} ${LIBJSONWRITER} ${LDFLAGS}

test check: ${TEST_BIN}
	${TEST_BIN}

# Prefer macOS 'leaks', then valgrind; ASAN=1 covers leaks via LeakSanitizer on
# Linux, so a bare run there is acceptable as a last resort.
test-leaks: ${TEST_BIN}
	@if command -v leaks >/dev/null 2>&1; then \
	  echo "leaks: ${TEST_BIN}"; \
	  MallocStackLogging=1 leaks --atExit -- ${TEST_BIN}; \
	elif command -v valgrind >/dev/null 2>&1; then \
	  valgrind --leak-check=full --error-exitcode=1 ${TEST_BIN}; \
	else \
	  echo "no leak checker found; running suite directly"; ${TEST_BIN}; \
	fi

# Strict-warnings syntax check of the library translation unit (jsonwriter.c
# pulls in utils.c + json_numeric.c via #include). Run per compiler: make strict CC=gcc
strict:
	@echo "  STRICT jsonwriter.c (${CC})"
	${CC} ${STRICT_CFLAGS} -I. -DINCLUDE_UTILS -fsyntax-only jsonwriter.c
	@echo "strict: clean (${CC})"

# libFuzzer target: harness + library TU in one clang invocation so coverage and
# sanitizer instrumentation reach the escaper and the writer.
${FUZZ_BIN}: ${FUZZ_SRC} jsonwriter.c jsonwriter.h utils.c json_numeric.c
	@mkdir -p `dirname "$@"`
	${FUZZ_CC} -std=gnu11 -I. -DINCLUDE_UTILS -g -O1 -fno-omit-frame-pointer \
	  -fsanitize=fuzzer,address,undefined -o $@ ${FUZZ_SRC} jsonwriter.c

fuzz: ${FUZZ_BIN}
	@echo "built ${FUZZ_BIN}"
	@echo "run e.g.: ${FUZZ_BIN} -max_total_time=60 ${FUZZ_CORPUS}"

# Portable replay driver: runs each argv file (or stdin) through the harness once.
# Builds with any toolchain, honours ASAN=1 -- CI smoke coverage and crash replay.
${FUZZ_STANDALONE_BIN}: ${FUZZ_SRC} jsonwriter.c jsonwriter.h utils.c json_numeric.c
	@mkdir -p `dirname "$@"`
	${CC} ${CFLAGS} -I. -DINCLUDE_UTILS -DJSW_FUZZ_STANDALONE -o $@ ${FUZZ_SRC} jsonwriter.c ${LDFLAGS}

fuzz-standalone: ${FUZZ_STANDALONE_BIN}
	@echo "built ${FUZZ_STANDALONE_BIN}"

print-builddir:
	@echo ${BUILD_DIR}

.PHONY: test check test-leaks strict fuzz fuzz-standalone print-builddir

# ------------------------------------------------- dockerized fuzzing (df)
# Run the libFuzzer target inside Linux so fuzzing works on macOS, including
# Apple Silicon, where Apple clang has no libFuzzer runtime. The image is
# multi-arch, so it runs natively (no emulation) on an arm64 host. Self-
# contained -- unlike zsv's df setup this needs no external repo, compose or yq.
# Findings persist on the host under ${DF_FINDINGS}. See `make df-help`.
DF_DOCKERFILE= ${TESTS_DIR}/Dockerfile.fuzz
DF_IMAGE?= jsonwriter-fuzz
DF_FINDINGS?= ${TESTS_DIR}/fuzz-findings
FUZZ_SECONDS?= 60
JOBS?= 1

# JOBS>1 uses libFuzzer's own parallelism (N worker processes in one container).
ifeq ($(JOBS),1)
  DF_JOBFLAGS=
else
  DF_JOBFLAGS= -jobs=${JOBS} -workers=${JOBS}
endif

df-help:
	@echo "Dockerized libFuzzer fuzzing (works on macOS / Apple Silicon):"
	@echo "  make df-build                             # build the fuzzing image"
	@echo "  make df-run [FUZZ_SECONDS=N] [JOBS=N]     # build + fuzz; findings -> ${DF_FINDINGS}"
	@echo "  make df-list-crashes                      # list crash/leak/oom/timeout files found"
	@echo "  make df-repro CRASH_FILE=<file>           # replay one finding inside the container"
	@echo "  make df-clean                             # remove the image and findings dir"
	@echo ""
	@echo "Examples:"
	@echo "  make df-run FUZZ_SECONDS=300 JOBS=4"
	@echo "  make df-repro CRASH_FILE=${DF_FINDINGS}/crash-abc123"

df-build:
	docker build -f ${DF_DOCKERFILE} -t ${DF_IMAGE} .

# Seed from the baked-in corpus plus a writable host corpus; crash artifacts and
# any new corpus units land under ${DF_FINDINGS} on the host.
# The findings dir is bind-mounted, so it must sit under a path Docker Desktop
# shares (the default set includes your home dir / a repo under /Users).
df-run: df-build
	@mkdir -p ${DF_FINDINGS}/corpus
	docker run --rm -v "$(abspath ${DF_FINDINGS}):/findings" -w /findings ${DF_IMAGE} \
	  -max_total_time=${FUZZ_SECONDS} -print_final_stats=1 -artifact_prefix=/findings/ \
	  ${DF_JOBFLAGS} /findings/corpus /src/tests/corpus
	@echo "findings (if any) under ${DF_FINDINGS} -- list them with: make df-list-crashes"

df-list-crashes:
	@find ${DF_FINDINGS} -type f \( -name 'crash-*' -o -name 'leak-*' \
	  -o -name 'oom-*' -o -name 'timeout-*' \) 2>/dev/null || true

df-repro:
	@if [ -z "${CRASH_FILE}" ]; then echo "usage: make df-repro CRASH_FILE=<file>"; exit 1; fi
	@if [ ! -f "${CRASH_FILE}" ]; then echo "no such file: ${CRASH_FILE}"; exit 1; fi
	docker run --rm -v "$(abspath $(dir ${CRASH_FILE})):/repro:ro" ${DF_IMAGE} \
	  "/repro/$(notdir ${CRASH_FILE})"

df-clean:
	-docker rmi -f ${DF_IMAGE} 2>/dev/null
	rm -rf ${DF_FINDINGS}

.PHONY: df-help df-build df-run df-list-crashes df-repro df-clean
