export BOARD ?= mspts430

export SRC = "src"
export SRC_ROOT = $(abspath $(SRC))
TOOLS = \

TOOLCHAINS = \
	gcc \
	clang \

include ext/maker/Makefile
