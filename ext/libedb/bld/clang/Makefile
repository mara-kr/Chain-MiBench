EXEC = libedb.a.bc

OBJECTS = \
	edb.o \
	io.o \

include $(MAKER_ROOT)/Makefile.clang

CFLAGS += \
	-I$(LIBMSPBUILTINS_ROOT)/src/include \
	-I$(WISP_BASE_ROOT) \
	-I ../src/include/libedb \

VPATH = ../src
