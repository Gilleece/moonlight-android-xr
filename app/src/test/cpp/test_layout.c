// The hover zones, the stand in screen and the Room tab's lanes, checked
// against answers that can be worked out by hand
#include "check.h"
#include "xr_layout.h"
#include "xr_shared.h"

// A 3 m picture at the video's usual shape, the size a screen starts at
static const float W = 3.0f;
static const float H = 3.0f * 9.0f / 16.0f;

static void testCornersFollowTheirArt(void) {
    float side = CORNER_FRAC * W;
    // The bracket is drawn centred half a bracket outside each corner, and
    // its centre is where the zone is centred too
    float outU = side * 0.5f / W;
    float outV = side * 0.5f / H;
    int corner = -1;
    CHECK(hoverTest(-outU, -outV, W, H, side, &corner) == HOVER_CORNER && corner == 0);
    CHECK(hoverTest(1.0f + outU, -outV, W, H, side, &corner) == HOVER_CORNER && corner == 1);
    CHECK(hoverTest(-outU, 1.0f + outV, W, H, side, &corner) == HOVER_CORNER && corner == 2);
    CHECK(hoverTest(1.0f + outU, 1.0f + outV, W, H, side, &corner) == HOVER_CORNER
          && corner == 3);

    // The corner itself, where the bracket's inner tip touches, is inside the
    // zone and so is the picture a little in from it
    CHECK(hoverTest(0.0f, 0.0f, W, H, side, &corner) == HOVER_CORNER && corner == 0);
    CHECK(hoverTest(0.01f, 0.01f, W, H, side, &corner) == HOVER_CORNER);

    // The zone reaches CORNER_HOVER halves of a bracket each way from its
    // centre and no further
    float reachU = side * CORNER_HOVER * 0.5f / W;
    CHECK(hoverTest(-outU - reachU * 0.98f, -outV, W, H, side, &corner) == HOVER_CORNER);
    CHECK(hoverTest(-outU - reachU * 1.02f, -outV, W, H, side, &corner) != HOVER_CORNER);
    CHECK(hoverTest(-outU + reachU * 1.02f, -outV, W, H, side, &corner) != HOVER_CORNER);

    // Along an edge away from the corners is the picture or its margin
    CHECK(hoverTest(0.5f, 0.0f, W, H, side, &corner) == HOVER_SCREEN);
    CHECK(hoverTest(0.5f, -outV, W, H, side, &corner) == HOVER_HALO);
}

static void testNoCornersWhereThereAreNone(void) {
    // A room that keeps its picture whole has no brackets, so the ray falls
    // through to the picture and its margin
    int corner = -1;
    float side = CORNER_FRAC * W;
    float outU = side * 0.5f / W;
    float outV = side * 0.5f / H;
    CHECK(hoverTest(-outU, -outV, W, H, 0.0f, &corner) == HOVER_HALO);
    CHECK(hoverTest(0.0f, 0.0f, W, H, 0.0f, &corner) == HOVER_SCREEN);
    CHECK(hoverTest(1.0f + outU, 1.0f + outV, W, H, 0.0f, &corner) == HOVER_HALO);
    CHECK(corner == -1);
}

static void testTheRestOfThePicture(void) {
    int corner = -1;
    float side = CORNER_FRAC * W;
    CHECK(hoverTest(0.5f, 0.5f, W, H, side, &corner) == HOVER_SCREEN);
    // The bar's zone under the bottom edge, and past the halo nothing at all
    CHECK(hoverTest(0.5f, 1.05f, W, H, side, &corner) == HOVER_BAR);
    CHECK(hoverTest(0.5f, 2.0f, W, H, side, &corner) == HOVER_NONE);
    CHECK(hoverTest(-0.6f, 0.5f, W, H, side, &corner) == HOVER_NONE);
}

static void testTheStandIn(void) {
    // Straight ahead of the seat at eye level, square to it, 3 m wide
    XrPosef pose = standInPose();
    CHECK_NEAR(pose.position.x, 0.0f, 1e-6);
    CHECK_NEAR(pose.position.y, 0.0f, 1e-6);
    CHECK_NEAR(pose.position.z, -STAND_IN_DISTANCE_M, 1e-6);
    CHECK_NEAR(pose.orientation.w, 1.0f, 1e-6);
    CHECK_NEAR(STAND_IN_WIDTH_M, 3.0f, 1e-6);

    // A ray from the seat lands on it where the placement says it should, so
    // the furniture hit tests follow what is drawn: straight ahead is the
    // middle, and aimed at the bar's place under it is the bar's zone
    XrPosef aim;
    aim.position.x = 0.0f;
    aim.position.y = 0.0f;
    aim.position.z = 0.0f;
    aim.orientation.x = 0.0f;
    aim.orientation.y = 0.0f;
    aim.orientation.z = 0.0f;
    aim.orientation.w = 1.0f;
    float u, v;
    CHECK(screenProject(aim, pose, STAND_IN_WIDTH_M, H, 0.0f, 0, &u, &v));
    CHECK_NEAR(u, 0.5f, 1e-5);
    CHECK_NEAR(v, 0.5f, 1e-5);

    float barY = -(H * 0.5f + STAND_IN_WIDTH_M * (BAR_GAP_FRAC + BAR_HEIGHT_FRAC * 0.5f));
    float pitch = atan2f(barY, STAND_IN_DISTANCE_M);
    aim.orientation.x = sinf(pitch * 0.5f);
    aim.orientation.w = cosf(pitch * 0.5f);
    CHECK(screenProject(aim, pose, STAND_IN_WIDTH_M, H, 0.0f, 0, &u, &v));
    int corner;
    CHECK(hoverTest(u, v, STAND_IN_WIDTH_M, H, 0.0f, &corner) == HOVER_BAR);
}

static void testRoomCornersLookTheSameEverywhere(void) {
    // At the stand in's distance a bracket is the size one is on a 3 m screen
    CHECK_NEAR(roomCornerSide(STAND_IN_DISTANCE_M), CORNER_FRAC * STAND_IN_WIDTH_M, 1e-6);
    // Further off it grows with the distance, so it subtends the same angle
    CHECK_NEAR(roomCornerSide(14.0f), CORNER_FRAC * STAND_IN_WIDTH_M * 14.0f / 3.0f, 1e-5);
    CHECK_NEAR(roomCornerSide(4.15f) / 4.15f, roomCornerSide(14.0f) / 14.0f, 1e-6);
    CHECK_NEAR(roomCornerSide(0.0f), 0.0f, 1e-6);
}

static void testLanes(void) {
    // Each room's starting brightness reads as the percent of the lane it sits at
    CHECK(lanePercent(ROOM_THEATER_BRIGHTNESS, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX) == 20);
    CHECK(lanePercent(ROOM_GRAND_CINEMA_BRIGHTNESS, ROOM_BRIGHTNESS_MIN,
                      ROOM_BRIGHTNESS_MAX) == 10);
    CHECK(lanePercent(ROOM_SYNTHWAVE_BRIGHTNESS, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX) == 30);
    CHECK(lanePercent(ROOM_LIGHT_DEFAULT, ROOM_LIGHT_MIN, ROOM_LIGHT_MAX) == 75);

    // Both ends, and a place off either end held at it
    CHECK(laneUnits(0.0f, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX) == ROOM_BRIGHTNESS_MIN);
    CHECK(laneUnits(1.0f, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX) == ROOM_BRIGHTNESS_MAX);
    CHECK(laneUnits(-0.3f, ROOM_SCREEN_MIN, ROOM_SCREEN_MAX) == ROOM_SCREEN_MIN);
    CHECK(laneUnits(1.4f, ROOM_SCREEN_MIN, ROOM_SCREEN_MAX) == ROOM_SCREEN_MAX);
    CHECK_NEAR(lanePlace(ROOM_BRIGHTNESS_MIN - 3, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX),
               0.0f, 1e-6);
    CHECK_NEAR(lanePlace(ROOM_LIGHT_MAX + 30, ROOM_LIGHT_MIN, ROOM_LIGHT_MAX), 1.0f, 1e-6);

    // Every unit on each lane survives the trip to a thumb's place and back,
    // which is what lets a thumb show exactly what gets written
    int lanes[3][2] = { { ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX },
                        { ROOM_LIGHT_MIN, ROOM_LIGHT_MAX },
                        { ROOM_SCREEN_MIN, ROOM_SCREEN_MAX } };
    int trips = 0;
    for (int lane = 0; lane < 3; lane++) {
        int min = lanes[lane][0];
        int max = lanes[lane][1];
        for (int units = min; units <= max; units++) {
            trips += laneUnits(lanePlace(units, min, max), min, max) == units;
        }
    }
    CHECK(trips == (ROOM_BRIGHTNESS_MAX - ROOM_BRIGHTNESS_MIN + 1)
                   + (ROOM_LIGHT_MAX - ROOM_LIGHT_MIN + 1)
                   + (ROOM_SCREEN_MAX - ROOM_SCREEN_MIN + 1));
}

static void testTheSizeClamp(void) {
    CHECK(roomScreenClamp(80, 1) == 80);
    CHECK(roomScreenClamp(10, 1) == ROOM_SCREEN_MIN);
    CHECK(roomScreenClamp(-40, 1) == ROOM_SCREEN_MIN);
    CHECK(roomScreenClamp(130, 1) == ROOM_SCREEN_MAX);
    // A room that keeps its picture whole hangs all of it whatever was saved
    CHECK(roomScreenClamp(40, 0) == ROOM_SCREEN_MAX);
    CHECK(roomScreenClamp(ROOM_SCREEN_MIN, 0) == ROOM_SCREEN_MAX);
}

static void testTheCornerDrag(void) {
    // Scaled from where the drag started, in whole percent
    CHECK(roomResizePercent(80, 1.0f, 1.0f) == 80);
    CHECK(roomResizePercent(80, 1.0f, 0.5f) == 40);
    CHECK(roomResizePercent(80, 1.0f, 1.1f) == 88);
    CHECK(roomResizePercent(60, 1.0f, 1.333f) == 80);
    // Taking hold of a bracket out past the corner moves nothing until the
    // hand does, however far out it was taken hold of
    CHECK(roomResizePercent(25, 1.41f, 1.41f) == 25);
    CHECK(roomResizePercent(25, 2.03f, 2.03f) == 25);
    CHECK(roomResizePercent(40, 2.0f, 1.0f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(40, 2.0f, 3.0f) == 60);
    // Never under a quarter of the room's screen or over all of it
    CHECK(roomResizePercent(80, 1.0f, 0.1f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(100, 1.0f, 0.25f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(100, 1.0f, 0.2f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(80, 1.0f, 2.0f) == ROOM_SCREEN_MAX);
    CHECK(roomResizePercent(25, 1.0f, 4.0f) == ROOM_SCREEN_MAX);
    // A ray behind the centre, or a grab begun on it, holds rather than
    // flipping the picture or dividing by nothing
    CHECK(roomResizePercent(80, 1.0f, -0.5f) == ROOM_SCREEN_MIN);
    CHECK(roomResizePercent(80, 0.0f, 0.05f) == 80);
    // Every step between lands on a whole percent inside the lane
    int inside = 1;
    for (int i = 1; i <= 400; i++) {
        int p = roomResizePercent(60, 1.2f, i * 0.01f);
        inside &= p >= ROOM_SCREEN_MIN && p <= ROOM_SCREEN_MAX;
    }
    CHECK(inside);
}

int main(void) {
    testCornersFollowTheirArt();
    testNoCornersWhereThereAreNone();
    testTheRestOfThePicture();
    testTheStandIn();
    testRoomCornersLookTheSameEverywhere();
    testLanes();
    testTheSizeClamp();
    testTheCornerDrag();
    return checksDone("xr_layout");
}
