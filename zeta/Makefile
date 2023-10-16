this-makefile := $(lastword $(MAKEFILE_LIST))
ZETA := $(realpath $(dir $(this-makefile)))
LINUX_ROOT:=${ZETA}/..

# FILES is a space-separated list of files.
#
# $ make format FILES="/crypto/sha256_generic.c /crypto/sha512_generic.c"
#
# Note the leading `/` (slash), which indicates the position of
# the input files with respect to the kernel source code.
FILES=

ALL_FILES:=$(addprefix ${LINUX_ROOT}, ${FILES})

all:
	@echo "Possible targets: format, lint"

check-env:
ifndef FILES
	$(error FILES env-var is not defined)
endif

format: check-env
	# Tested with astyle v3.8.4
	astyle --project=${ZETA}/.astylerc ${ALL_FILES}
	# Tested with clang-format v17
	clang-format --verbose -i --style=file:${ZETA}/.clang-format ${ALL_FILES}

lint: check-env
	# Tested with cpplint v1.5.5
	cpplint --filter=-whitespace/braces,-readability/casting,-build/include_subdir ${ALL_FILES}
