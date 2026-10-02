# Builds and runs the tests for the renderer modules that have no GL, OpenXR
# or Android in them, with whatever C compiler the machine has.
#
#     make -C app/src/test/cpp test

CC ?= cc
CFLAGS ?= -std=gnu11 -Wall -Werror -O1 -g
SRC := ../../main/jni/xr-renderer
AUDIO := ../../main/jni/xr-audio
INCLUDES := -Istubs -I$(SRC)
OUT := build

TESTS := $(OUT)/test_math $(OUT)/test_depthmap $(OUT)/test_atlas $(OUT)/test_roommesh \
         $(OUT)/test_layout $(OUT)/test_audio $(OUT)/test_rate $(OUT)/test_gate \
         $(OUT)/test_pinch $(OUT)/test_notice $(OUT)/test_glow $(OUT)/test_grade \
         $(OUT)/test_keys $(OUT)/test_controller $(OUT)/test_headaim $(OUT)/test_gamepad

.PHONY: test clean

test: $(TESTS)
	@for t in $(TESTS); do ./$$t || exit 1; done

$(OUT)/test_math: test_math.c check.h $(SRC)/xr_math.c $(SRC)/xr_math.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_math.c $(SRC)/xr_math.c -lm

$(OUT)/test_depthmap: test_depthmap.c check.h $(SRC)/xr_depthmap.c $(SRC)/xr_depthmap.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_depthmap.c $(SRC)/xr_depthmap.c -lm

# Reads the atlases that ship as well, relative to this directory
$(OUT)/test_atlas: test_atlas.c check.h $(SRC)/xr_atlas.c $(SRC)/xr_atlas.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_atlas.c $(SRC)/xr_atlas.c -lm

# Reads the room meshes that ship as well, relative to this directory
$(OUT)/test_roommesh: test_roommesh.c check.h $(SRC)/xr_roommesh.c $(SRC)/xr_roommesh.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_roommesh.c $(SRC)/xr_roommesh.c -lm

# The hover zones, the stand in, the Room tab's lanes and the 3D tab's track
# and presets, with the screen maths the stand in is hit tested through
$(OUT)/test_layout: test_layout.c check.h $(SRC)/xr_layout.c $(SRC)/xr_layout.h $(SRC)/xr_math.c \
                    $(SRC)/xr_math.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_layout.c $(SRC)/xr_layout.c $(SRC)/xr_math.c -lm

# The display refresh rate choice, the frame budget that steps it down, and
# the depth rate governor and capture gate spent before it
$(OUT)/test_rate: test_rate.c check.h $(SRC)/xr_rate.c $(SRC)/xr_rate.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_rate.c $(SRC)/xr_rate.c -lm

# Who may point, the controllers' own clocks and the gaze bridge
$(OUT)/test_gate: test_gate.c check.h $(SRC)/xr_gate.c $(SRC)/xr_gate.h $(SRC)/xr_math.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_gate.c $(SRC)/xr_gate.c -lm

# The pinch and its hold, the ring finger lock gesture, and the drag the
# eyes start
$(OUT)/test_pinch: test_pinch.c check.h $(SRC)/xr_pinch.c $(SRC)/xr_pinch.h $(SRC)/xr_math.c \
                   $(SRC)/xr_math.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_pinch.c $(SRC)/xr_pinch.c $(SRC)/xr_math.c -lm

# The panels' fades and the splash's timing
$(OUT)/test_notice: test_notice.c check.h $(SRC)/xr_notice.c $(SRC)/xr_notice.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_notice.c $(SRC)/xr_notice.c -lm

# The glow's steady luma, its roll off along a dark edge and the edge pass
# built from them
$(OUT)/test_glow: test_glow.c check.h $(SRC)/xr_glow.c $(SRC)/xr_glow.h $(SRC)/xr_math.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_glow.c $(SRC)/xr_glow.c -lm

# The picture grade: its lanes, the identity at the ticks, the clamp and a
# pixel worked by hand
$(OUT)/test_grade: test_grade.c check.h $(SRC)/xr_grade.c $(SRC)/xr_grade.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_grade.c $(SRC)/xr_grade.c -lm

# The keyboard's sheets and the Ctrl, Alt and Win it holds
$(OUT)/test_keys: test_keys.c check.h $(SRC)/xr_keys.c $(SRC)/xr_keys.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_keys.c $(SRC)/xr_keys.c -lm

# When the ray's beam is drawn, when a controller's model is, and where the
# model goes
$(OUT)/test_controller: test_controller.c check.h $(SRC)/xr_controller.c $(SRC)/xr_controller.h \
                        $(SRC)/xr_math.c $(SRC)/xr_math.h $(SRC)/xr_gate.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_controller.c $(SRC)/xr_controller.c $(SRC)/xr_math.c -lm

# The head turning the mouse, the frames it drops, and the controller's
# pointer moving the mouse by how far its point moves
$(OUT)/test_headaim: test_headaim.c check.h $(SRC)/xr_headaim.c $(SRC)/xr_headaim.h $(SRC)/xr_shared.h \
                     $(SRC)/xr_math.c $(SRC)/xr_math.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_headaim.c $(SRC)/xr_headaim.c $(SRC)/xr_math.c -lm

# The controllers as one pad: the mapping, the dead zones, what is held back
# and the switch between the pad and the pointer
$(OUT)/test_gamepad: test_gamepad.c check.h $(SRC)/xr_gamepad.c $(SRC)/xr_gamepad.h $(SRC)/xr_shared.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ test_gamepad.c $(SRC)/xr_gamepad.c -lm

# The audio module ships at -O2 and this test prints what its kernels cost, so
# it is built the way the app builds it
$(OUT)/test_audio: test_audio.c check.h $(AUDIO)/xr_audio.c $(AUDIO)/xr_audio.h
	@mkdir -p $(OUT)
	$(CC) $(CFLAGS) -O2 $(INCLUDES) -I$(AUDIO) -o $@ test_audio.c $(AUDIO)/xr_audio.c -lm

clean:
	rm -rf $(OUT)
