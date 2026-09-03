# Builds and runs the tests for the renderer modules that have no GL, OpenXR
# or Android in them, with whatever C compiler the machine has.
#
#     make -C app/src/test/cpp test

CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Werror -O1 -g
SRC := ../../main/jni/xr-renderer
INCLUDES := -Istubs -I$(SRC)
OUT := build

TESTS := $(OUT)/test_math $(OUT)/test_depthmap

.PHONY: test clean

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

$(OUT)/test_math: test_math.c check.h $(SRC)/xr_math.c $(SRC)/xr_math.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_math.c $(SRC)/xr_math.c -lm

$(OUT)/test_depthmap: test_depthmap.c check.h $(SRC)/xr_depthmap.c $(SRC)/xr_depthmap.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_depthmap.c $(SRC)/xr_depthmap.c -lm

clean:
	rm -rf $(OUT)
