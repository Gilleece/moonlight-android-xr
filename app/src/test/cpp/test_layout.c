// The hover zones, the stand in screen, the Room tab's lanes and the 3D tab's
// depth track and presets, checked against answers that can be worked out by
// hand
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

static XrPosef poseAt(float x, float y, float z) {
    XrPosef pose;
    pose.orientation.x = 0.0f;
    pose.orientation.y = 0.0f;
    pose.orientation.z = 0.0f;
    pose.orientation.w = 1.0f;
    pose.position.x = x;
    pose.position.y = y;
    pose.position.z = z;
    return pose;
}

static Vec3 posOf(XrPosef pose) {
    Vec3 p = { pose.position.x, pose.position.y, pose.position.z };
    return p;
}

static float lengthOf(Vec3 v) {
    return sqrtf(vecDot(v, v));
}

// A point in a frame's own flat coordinates, out in the space it hangs in
static Vec3 framePoint(const BarFrame* frame, Vec3 local) {
    Vec3 r = quatRotate(frame->pose.orientation, local);
    Vec3 p = { frame->pose.position.x + r.x, frame->pose.position.y + r.y,
               frame->pose.position.z + r.z };
    return p;
}

// The middle of the move bar, where the row of buttons is centred
static Vec3 barMiddle(const BarFrame* frame) {
    Vec3 local = { 0.0f, -(frame->height * 0.5f + frame->width * BAR_DROP_FRAC), 0.0f };
    return framePoint(frame, local);
}

// A ray from the seat straight at a point
static XrPosef aimAt(Vec3 target) {
    Vec3 dir = vecNorm(target);
    // -z turned onto dir: the axis is their cross product
    Vec3 fwd = { 0.0f, 0.0f, -1.0f };
    Vec3 axis = vecCross(fwd, dir);
    float s = lengthOf(axis);
    float angle = atan2f(s, vecDot(fwd, dir));
    XrPosef aim = poseAt(0.0f, 0.0f, 0.0f);
    if (s > 1e-6f) {
        aim.orientation = axisAngleQuat(vecNorm(axis), angle);
    }
    return aim;
}

// How wide the bar looks from the seat: its width over its distance
static float barLooks(const BarFrame* frame) {
    return frame->width * BAR_WIDTH_FRAC / lengthOf(barMiddle(frame));
}

// Each button aimed at from the seat is hit, on the frame it was placed on,
// and none of the others is
static void checkButtonsHitWhereTheyHang(const BarFrame* frame) {
    for (int slot = 0; slot < BAR_SLOTS; slot++) {
        Vec3 local;
        float side;
        barSlotPlacement(slot, frame->width, frame->height, &local, &side);
        float yaw = 0.0f;
        curveLocal(&local, frame->radius, frame->curved, &yaw);
        float u, v;
        CHECK(screenProject(aimAt(framePoint(frame, local)), frame->pose, frame->width,
                            frame->height, frame->radius, frame->curved, &u, &v));
        for (int other = 0; other < BAR_SLOTS; other++) {
            CHECK(barSlotHit(other, u, v, frame->width, frame->height) == (other == slot));
        }
    }
    // And the middle of the row is the move bar's zone
    float u, v;
    int corner;
    CHECK(screenProject(aimAt(barMiddle(frame)), frame->pose, frame->width, frame->height,
                        frame->radius, frame->curved, &u, &v));
    CHECK(hoverTest(u, v, frame->width, frame->height, 0.0f, &corner) == HOVER_BAR);
}

static void testTheBarUnderARoomsPicture(void) {
    const float aspect = 9.0f / 16.0f;
    // A room picture that is the stand in hangs the bar exactly where the
    // stand in did
    BarFrame same = roomBarFrame(standInPose(), STAND_IN_WIDTH_M * aspect, aspect);
    CHECK_NEAR(same.width, STAND_IN_WIDTH_M, 1e-4);
    CHECK_NEAR(same.height, STAND_IN_WIDTH_M * aspect, 1e-4);
    CHECK_NEAR(same.pose.position.y, 0.0f, 1e-5);
    CHECK_NEAR(same.pose.position.z, -STAND_IN_DISTANCE_M, 1e-6);
    CHECK(!same.curved);
    float standInLooks = barLooks(&same);

    // The Home Theater at 100 and 50 percent and Synthwave at 25 and 100, as
    // the rooms hang them from the seat
    struct { float x, y, z, width; } rooms[] = {
        { 0.0f, 0.05f, -4.152f, 3.6f },
        { 0.0f, 0.05f, -4.152f, 1.8f },
        { 0.0f, 0.05f, -4.152f, 2.88f },
        { 0.0f, 2.95f, -13.9f, 3.5f },
        { 0.0f, 2.95f, -13.9f, 14.0f },
    };
    float lastBarY = 1e9f;
    for (int i = 0; i < (int)(sizeof(rooms) / sizeof(rooms[0])); i++) {
        float pictureH = rooms[i].width * aspect;
        XrPosef picture = poseAt(rooms[i].x, rooms[i].y, rooms[i].z);
        BarFrame frame = roomBarFrame(picture, pictureH, aspect);
        // Flat, in the picture's plane, centred on it, its bottom edge the
        // picture's bottom edge
        CHECK(!frame.curved);
        CHECK_NEAR(frame.pose.position.z, rooms[i].z, 1e-5);
        CHECK_NEAR(frame.pose.position.x, rooms[i].x, 1e-5);
        CHECK_NEAR(frame.pose.position.y - frame.height * 0.5f,
                   rooms[i].y - pictureH * 0.5f, 1e-4);
        CHECK_NEAR(frame.height, frame.width * aspect, 1e-5);
        // The bar looks the size it does on the stand in
        CHECK_NEAR(barLooks(&frame) / standInLooks, 1.0f, 1e-3);
        // Just under the picture: the gap to the bar is the stand in's gap at
        // this distance, not the full size picture's
        Vec3 bar = barMiddle(&frame);
        float bottom = rooms[i].y - pictureH * 0.5f;
        CHECK(bar.y < bottom);
        CHECK_NEAR(bottom - bar.y, frame.width * BAR_DROP_FRAC, 1e-4);
        checkButtonsHitWhereTheyHang(&frame);
        // The first two are the same room, 100 then 50 percent: the smaller
        // picture brings the bar up with its bottom edge
        if (i == 1) {
            CHECK(bar.y > lastBarY + 0.4f);
        }
        lastBarY = bar.y;
    }

    // Further off it is larger, by the distance
    BarFrame near = roomBarFrame(poseAt(0.0f, 0.0f, -4.0f), 1.0f, aspect);
    BarFrame far = roomBarFrame(poseAt(0.0f, 0.0f, -16.0f), 1.0f, aspect);
    CHECK(far.width > 3.5f * near.width && far.width < 4.5f * near.width);
    CHECK_NEAR(barLooks(&near), barLooks(&far), 1e-4);
}

// A picture turned and tipped keeps the frame in its own plane and the two
// bottom edges together
static void testTheBarUnderATurnedPicture(void) {
    const float aspect = 9.0f / 16.0f;
    Vec3 up = { 0.0f, 1.0f, 0.0f };
    Vec3 right = { 1.0f, 0.0f, 0.0f };
    XrPosef picture = poseAt(-2.0f, 0.6f, -5.0f);
    picture.orientation = quatMul(axisAngleQuat(up, 0.4f), axisAngleQuat(right, 0.2f));
    float pictureH = 2.0f * aspect;
    BarFrame frame = roomBarFrame(picture, pictureH, aspect);

    Vec3 back = { 0.0f, 0.0f, 1.0f };
    Vec3 normal = quatRotate(picture.orientation, back);
    Vec3 offset = vecSub(posOf(frame.pose), posOf(picture));
    CHECK_NEAR(vecDot(offset, normal), 0.0f, 1e-5);
    CHECK_NEAR(frame.pose.orientation.x, picture.orientation.x, 1e-6);
    CHECK_NEAR(frame.pose.orientation.w, picture.orientation.w, 1e-6);

    Vec3 down = { 0.0f, -1.0f, 0.0f };
    Vec3 pictureDown = quatRotate(picture.orientation, down);
    Vec3 pictureEdge = { picture.position.x + pictureDown.x * pictureH * 0.5f,
                         picture.position.y + pictureDown.y * pictureH * 0.5f,
                         picture.position.z + pictureDown.z * pictureH * 0.5f };
    Vec3 frameEdgeLocal = { 0.0f, -frame.height * 0.5f, 0.0f };
    Vec3 frameEdge = framePoint(&frame, frameEdgeLocal);
    CHECK_NEAR(lengthOf(vecSub(frameEdge, pictureEdge)), 0.0f, 1e-4);
    checkButtonsHitWhereTheyHang(&frame);
}

// Outside a room the frame is the picture as drawn. A cylinder held under a
// full turn draws it smaller, and the row comes up with the picture's bottom
// edge rather than staying where the full size picture would have put it.
static void testTheBarOnACurvedPicture(void) {
    const float aspect = 9.0f / 16.0f;
    // Wrapped right round at 0.2 m, which the clamp draws at 42 percent
    float width = 3.0f;
    float radius = 0.2f;
    float fit = cylinderFit(width, radius);
    CHECK(fit < 0.5f && fit > 0.4f);
    BarFrame frame;
    frame.pose = poseAt(0.0f, 0.0f, -0.2f);
    frame.width = width * fit;
    frame.height = frame.width * aspect;
    frame.radius = radius;
    frame.curved = 1;
    checkButtonsHitWhereTheyHang(&frame);

    // A full curve at 2 m, no clamp: the outer buttons follow the surface, so
    // a ray at each lands on it
    frame.pose = poseAt(0.0f, 0.0f, -2.0f);
    frame.width = 3.0f;
    frame.height = frame.width * aspect;
    frame.radius = 2.0f;
    checkButtonsHitWhereTheyHang(&frame);
    // On the surface the outermost lands in the middle of its zone. Where the
    // flat row used to hang it, a ray at it landed a good way off the middle,
    // near the zone's edge.
    Vec3 flat;
    float side;
    barSlotPlacement(BAR_SLOT_STEREO, frame.width, frame.height, &flat, &side);
    float cu = 0.5f + flat.x / frame.width;
    float cv = 0.5f - flat.y / frame.height;
    float halfU = side * HOVER_MARGIN * 0.5f / frame.width;
    float halfV = side * HOVER_MARGIN * 0.5f / frame.height;
    Vec3 curved = flat;
    curveLocal(&curved, frame.radius, 1, NULL);
    float u, v;
    CHECK(screenProject(aimAt(framePoint(&frame, curved)), frame.pose, frame.width,
                        frame.height, frame.radius, 1, &u, &v));
    // Within the 5 mm the button stands proud of the surface
    CHECK_NEAR(u, cu, 3e-3);
    CHECK_NEAR(v, cv, 3e-3);
    CHECK(screenProject(aimAt(framePoint(&frame, flat)), frame.pose, frame.width, frame.height,
                        frame.radius, 1, &u, &v));
    CHECK(fabsf(u - cu) > 0.3f * halfU && fabsf(v - cv) > 0.5f * halfV);

    // Flat, the frame is the picture and the row is where it always was
    frame.radius = 0.0f;
    frame.curved = 0;
    checkButtonsHitWhereTheyHang(&frame);
}

// The row's order and spacing: the pill in the middle, the picker and cog
// either side of it, then two each side further out, all on one line under
// the frame and never overlapping
static void testTheRowsOrder(void) {
    const float w = 3.0f, h = w * 9.0f / 16.0f;
    static const int LEFT_OUT[4] = { BAR_SLOT_ENV, BAR_SLOT_EXIT, BAR_SLOT_PAD, BAR_SLOT_AIM };
    static const int RIGHT_OUT[4] = { BAR_SLOT_COG, BAR_SLOT_KB, BAR_SLOT_RAY, BAR_SLOT_STEREO };
    float lastL = -w * BAR_WIDTH_FRAC * 0.5f, lastR = w * BAR_WIDTH_FRAC * 0.5f;
    for (int i = 0; i < 4; i++) {
        Vec3 l, r;
        float sideL, sideR;
        barSlotPlacement(LEFT_OUT[i], w, h, &l, &sideL);
        barSlotPlacement(RIGHT_OUT[i], w, h, &r, &sideR);
        CHECK_NEAR(l.x, -r.x, 1e-6);
        CHECK_NEAR(l.y, -(h * 0.5f + w * BAR_DROP_FRAC), 1e-6);
        CHECK_NEAR(r.y, l.y, 1e-6);
        CHECK(l.x + sideL * 0.5f < lastL && r.x - sideR * 0.5f > lastR);
        lastL = l.x - sideL * 0.5f;
        lastR = r.x + sideR * 0.5f;
    }
    // Every button is clear of the frame's bottom edge
    for (int slot = 0; slot < BAR_SLOTS; slot++) {
        Vec3 p;
        float side;
        barSlotPlacement(slot, w, h, &p, &side);
        CHECK(p.y + side * HOVER_MARGIN * 0.5f < -h * 0.5f);
    }
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

static void testTheDepthTrack(void) {
    // Every step of the track survives the trip to the fraction the warp
    // takes and back, and to a thumb's place and back, so what the thumb
    // shows is what gets written
    int trips = 0;
    for (int units = 0; units <= COG_SEP_STEPS; units++) {
        trips += separationUnits(separationOf(units)) == units;
        trips += laneUnits(lanePlace(units, 0, COG_SEP_STEPS), 0, COG_SEP_STEPS) == units;
    }
    CHECK(trips == 2 * (COG_SEP_STEPS + 1));
    CHECK(separationUnits(COG_SEP_MAX) == COG_SEP_STEPS);
    CHECK_NEAR(separationOf(5), 0.005f, 1e-7);

    // A drag lands on the nearest step and never off either end
    CHECK(laneUnits(0.39f, 0, COG_SEP_STEPS) == 6);
    CHECK(laneUnits(0.36f, 0, COG_SEP_STEPS) == 5);
    CHECK(laneUnits(-0.2f, 0, COG_SEP_STEPS) == 0);
    CHECK(laneUnits(1.3f, 0, COG_SEP_STEPS) == COG_SEP_STEPS);

    // The debug property can ask for more than the track shows, and that is
    // read as it is rather than pulled back onto the track
    CHECK(separationUnits(0.04f) == 40);
}

static void testThePresetAccent(void) {
    // ZipDepth's three, and MiDaS's
    int zip[COG_PRESET_CELLS] = { 3, 6, 9 };
    int midas[COG_PRESET_CELLS] = { 2, 5, 8 };
    CHECK(cogPresetAt(3, zip) == COG_PRESET_COMFORT);
    CHECK(cogPresetAt(6, zip) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(9, zip) == COG_PRESET_STRONG);
    CHECK(cogPresetAt(2, midas) == COG_PRESET_COMFORT);
    CHECK(cogPresetAt(5, midas) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(8, midas) == COG_PRESET_STRONG);

    // Anywhere else on the track, or past it, rings nothing
    int elsewhere = 0;
    for (int units = 0; units <= COG_SEP_STEPS; units++) {
        if (units != 3 && units != 6 && units != 9) {
            elsewhere += cogPresetAt(units, zip) == -1;
        }
    }
    CHECK(elsewhere == COG_SEP_STEPS + 1 - 3);
    CHECK(cogPresetAt(40, zip) == -1);

    // A default at an end clamps another preset onto it, and that is Balanced
    int low[COG_PRESET_CELLS] = { 0, 0, 3 };
    int high[COG_PRESET_CELLS] = { 12, 15, 15 };
    CHECK(cogPresetAt(0, low) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(15, high) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(3, low) == COG_PRESET_STRONG);

    // Read back off the fraction the warp holds, the way the ring reads it
    CHECK(cogPresetAt(separationUnits(separationOf(9)), zip) == COG_PRESET_STRONG);
    CHECK(cogPresetAt(separationUnits(0.0061f), zip) == COG_PRESET_BALANCED);
    CHECK(cogPresetAt(separationUnits(0.0068f), zip) == -1);
}

// Every tab's rows fit inside the panel below the tab bar, with their hit
// bands apart and every cell drawn inside its own band
static void testTheRowsFit(void) {
    int counts[COG_TAB_COUNT] = {
        COG_SCREEN_ROW_COUNT, COG_DISPLAY_SLIDER_ROW + 1, COG_ROW3D_COUNT, PICTURE_VALUES, 0
    };
    for (int tab = 0; tab < COG_TAB_COUNT; tab++) {
        // The About tab has no rows, only its button
        if (counts[tab] == 0) {
            continue;
        }
        float half = cogRowHalf(tab);
        CHECK(cogCellHalf(tab) < half);
        CHECK(cogRowV(tab, 0) - half >= COG_TAB_BAR_B);
        for (int row = 1; row < counts[tab]; row++) {
            CHECK(cogRowV(tab, row) - half >= cogRowV(tab, row - 1) + half - 1e-6f);
        }
        float last = cogRowV(tab, counts[tab] - 1);
        // The thumb on a track is 0.085 of the panel's height across
        CHECK(last + 0.0425f < 1.0f - 0.03f);
        CHECK(last + half <= 1.0f);
    }
    // The display tab's thirteen rows, head aim's after head locked and the
    // controllers' pointer or gamepad after that, the glow level track last
    CHECK(COG_DISPLAY_SLIDER_ROW == 12);
    CHECK(COG_OPTION_HEAD_AIM == COG_OPTION_HEAD_LOCK + 1);
    CHECK(COG_OPTION_GAMEPAD == COG_OPTION_HEAD_AIM + 1);
    CHECK(COG_OPTION_POINTER_SLEEP == COG_OPTION_GAMEPAD + 1);
    CHECK_NEAR(cogRowV(COG_TAB_DISPLAY, 0), 0.197, 1e-6);
    CHECK_NEAR(cogRowV(COG_TAB_DISPLAY, COG_DISPLAY_SLIDER_ROW), 0.917, 1e-5);
    // Its first band starts under the tab bar, and its cells are shallower
    // than the rows are apart
    CHECK(cogRowV(COG_TAB_DISPLAY, 0) - cogRowHalf(COG_TAB_DISPLAY) >= COG_TAB_BAR_B);
    CHECK(2.0f * cogCellHalf(COG_TAB_DISPLAY) < COG_DISPLAY_ROW_STEP);
    // The screen tab's eight, head aim's two last, all clear of the reset
    // button, with chevrons and ticks shallower than the rows are apart
    CHECK(COG_SCREEN_ROW_COUNT == 8);
    CHECK(COG_SLIDER_AIM_SENSITIVITY == COG_SLIDER_COUNT);
    CHECK(COG_SLIDER_AIM_DEADZONE == COG_SLIDER_COUNT + 1);
    CHECK_NEAR(cogRowV(COG_TAB_SCREEN, 0), 0.22, 1e-6);
    CHECK_NEAR(cogRowV(COG_TAB_SCREEN, COG_SLIDER_AIM_DEADZONE), 0.815, 1e-5);
    CHECK(cogRowV(COG_TAB_SCREEN, COG_SCREEN_ROW_COUNT - 1) + cogRowHalf(COG_TAB_SCREEN)
          < COG_RESET_T);
    CHECK(2.0f * cogCellHalf(COG_TAB_SCREEN) < COG_SCREEN_ROW_STEP);
    // Its thumbs leave a gap between neighbouring rows, and every other tab
    // keeps the size its thumbs always had
    CHECK(cogThumbSize(COG_TAB_SCREEN) < COG_SCREEN_ROW_STEP);
    CHECK_NEAR(cogThumbSize(COG_TAB_DISPLAY), 0.085, 1e-6);
    CHECK_NEAR(cogThumbSize(COG_TAB_3D), 0.085, 1e-6);
    CHECK_NEAR(cogThumbSize(COG_TAB_COUNT), 0.085, 1e-6);
    // The other tabs keep the rows they always had
    CHECK_NEAR(cogRowV(COG_TAB_3D, 3), COG_ROW_V0 + 3 * COG_ROW_STEP, 1e-6);
    CHECK_NEAR(cogRowV(COG_TAB_COUNT, 2), COG_ROW_V0 + 2 * COG_ROW_STEP, 1e-6);
    CHECK_NEAR(cogCellHalf(COG_TAB_3D), COG_CELL_HALF, 1e-6);
    // The Picture tab is the fourth, with the screen tab's spacing, and its
    // last row's band ends well clear of the reset button
    CHECK(COG_TAB_PICTURE == 3 && COG_TAB_ABOUT == 4 && COG_TAB_COUNT == 5);
    CHECK_NEAR(cogRowV(COG_TAB_PICTURE, PICTURE_SATURATION),
               COG_ROW_V0 + PICTURE_SATURATION * COG_ROW_STEP, 1e-6);
    CHECK(cogRowV(COG_TAB_PICTURE, PICTURE_VALUES - 1) + cogRowHalf(COG_TAB_PICTURE)
          < COG_RESET_T);
    // Every room sheet follows the tabs, one per tab and the fixed Room tab,
    // then the screen and display tabs for a screen not locked to the head
    CHECK(COG_ART_ROOM == COG_TAB_COUNT);
    CHECK(COG_ART_SCREEN_WORLD == COG_ART_ROOM + COG_TAB_COUNT + 1);
    CHECK(COG_ART_DISPLAY_WORLD == COG_ART_SCREEN_WORLD + 1);
    CHECK(COG_ART_COUNT == COG_ART_DISPLAY_WORLD + 1);
    // The strip beside the tracks reaches every Picture and Room row, and
    // head aim's two on the Screen tab
    float stripB = COG_READOUT_T + (float)COG_READOUT_TEX_H / (float)COG_TEX_H;
    for (int row = 0; row < PICTURE_VALUES; row++) {
        CHECK(cogRowV(COG_TAB_PICTURE, row) - COG_CELL_HALF > COG_READOUT_T);
        CHECK(cogRowV(COG_TAB_PICTURE, row) + COG_CELL_HALF < stripB);
    }
    for (int row = 0; row < COG_ROOM_ROW_COUNT; row++) {
        CHECK(cogRowV(COG_TAB_COUNT, row) + COG_CELL_HALF < stripB);
    }
    for (int row = COG_SLIDER_AIM_SENSITIVITY; row <= COG_SLIDER_AIM_DEADZONE; row++) {
        CHECK(cogRowV(COG_TAB_SCREEN, row) - COG_SCREEN_CELL_HALF > COG_READOUT_T);
        CHECK(cogRowV(COG_TAB_SCREEN, row) + COG_SCREEN_CELL_HALF < stripB);
    }
    CHECK(stripB <= 1.0f);
}

// The step buttons at each end of a track, the run between them, and where a
// press of one lands
static void testTheTrackParts(void) {
    float mid = (COG_TRACK_L + COG_TRACK_R) * 0.5f;
    CHECK(cogTrackPart(mid) == TRACK_PART_RUN);
    CHECK(cogTrackPart(COG_TRACK_L + COG_CHEVRON_W * 0.5f) == TRACK_PART_DOWN);
    CHECK(cogTrackPart(COG_TRACK_R - COG_CHEVRON_W * 0.5f) == TRACK_PART_UP);
    // The buttons reach a little past the track's ends, and no further
    CHECK(cogTrackPart(COG_TRACK_L - COG_CHEVRON_REACH * 0.9f) == TRACK_PART_DOWN);
    CHECK(cogTrackPart(COG_TRACK_R + COG_CHEVRON_REACH * 0.9f) == TRACK_PART_UP);
    CHECK(cogTrackPart(COG_TRACK_L - COG_CHEVRON_REACH * 1.1f) == TRACK_PART_NONE);
    CHECK(cogTrackPart(COG_TRACK_R + COG_CHEVRON_REACH * 1.1f) == TRACK_PART_NONE);
    // The run's zone stops where a button starts, so a press on a button is
    // never a jump along the track
    CHECK(cogTrackPart(COG_TRACK_L + COG_CHEVRON_W * 1.01f) == TRACK_PART_RUN);
    CHECK(cogTrackPart(COG_TRACK_R - COG_CHEVRON_W * 1.01f) == TRACK_PART_RUN);
    CHECK(cogTrackPart(0.1f) == TRACK_PART_NONE);
    CHECK(cogTrackPart(0.99f) == TRACK_PART_NONE);

    // The run sits inside the buttons with room for a thumb at either end
    float thumbHalfU = 0.085f * 0.5f * (float)COG_TEX_H / (float)COG_TEX_W;
    CHECK(COG_RUN_L - thumbHalfU > COG_TRACK_L + COG_CHEVRON_W);
    CHECK(COG_RUN_R + thumbHalfU < COG_TRACK_R - COG_CHEVRON_W);
    CHECK_NEAR(cogRunPlace(COG_RUN_L), 0.0, 1e-6);
    CHECK_NEAR(cogRunPlace(COG_RUN_R), 1.0, 1e-6);
    CHECK_NEAR(cogRunPlace(COG_TRACK_L), 0.0, 1e-6);
    CHECK_NEAR(cogRunPlace(COG_TRACK_R), 1.0, 1e-6);
    CHECK_NEAR(cogRunPlace(cogRunU(0.37f)), 0.37, 1e-6);
}

static void testTheStepsLandOnTheGrid(void) {
    // On a step, one either way
    CHECK(cogStepIndex(5.0f / 20.0f, 20, 1) == 6);
    CHECK(cogStepIndex(5.0f / 20.0f, 20, -1) == 4);
    // Read back a hair off it, still that step
    CHECK(cogStepIndex(5.0f / 20.0f - 1e-6f, 20, 1) == 6);
    CHECK(cogStepIndex(5.0f / 20.0f + 1e-6f, 20, -1) == 4);
    // Between two, the nearer one that way
    CHECK(cogStepIndex(5.4f / 20.0f, 20, 1) == 6);
    CHECK(cogStepIndex(5.4f / 20.0f, 20, -1) == 5);
    CHECK(cogStepIndex(5.6f / 20.0f, 20, 1) == 6);
    CHECK(cogStepIndex(5.6f / 20.0f, 20, -1) == 5);
    // Held to the ends
    CHECK(cogStepIndex(0.0f, 20, -1) == 0);
    CHECK(cogStepIndex(1.0f, 20, 1) == 20);
    CHECK(cogStepIndex(0.5f, 0, 1) == 0);
}

static void testEveryTrackHasItsSteps(void) {
    for (int row = 0; row < COG_SCREEN_ROW_COUNT; row++) {
        CHECK(cogTrackSteps(COG_TAB_SCREEN, row) > 0);
    }
    CHECK(cogTrackSteps(COG_TAB_SCREEN, COG_SCREEN_ROW_COUNT) == 0);

    // Head aim's two step a whole unit a press, and each default, where its
    // tick is, is a step
    int aimLanes[2][4] = {
        { COG_SLIDER_AIM_SENSITIVITY, HEAD_AIM_SENSITIVITY_MIN, HEAD_AIM_SENSITIVITY_MAX,
          HEAD_AIM_SENSITIVITY_DEFAULT },
        { COG_SLIDER_AIM_DEADZONE, HEAD_AIM_DEADZONE_MIN, HEAD_AIM_DEADZONE_MAX,
          HEAD_AIM_DEADZONE_DEFAULT }
    };
    for (int l = 0; l < 2; l++) {
        int min = aimLanes[l][1];
        int max = aimLanes[l][2];
        int def = aimLanes[l][3];
        int steps = cogTrackSteps(COG_TAB_SCREEN, aimLanes[l][0]);
        CHECK(steps == max - min);
        int up = cogStepIndex(lanePlace(def, min, max), steps, 1);
        CHECK(laneUnits((float)up / (float)steps, min, max) == def + 1);
        int down = cogStepIndex(lanePlace(def, min, max), steps, -1);
        CHECK(laneUnits((float)down / (float)steps, min, max) == def - 1);
        for (int units = min; units <= max; units++) {
            CHECK(laneUnits(lanePlace(units, min, max), min, max) == units);
        }
    }
    CHECK(cogTrackSteps(COG_TAB_DISPLAY, COG_DISPLAY_SLIDER_ROW) == 20);
    CHECK(cogTrackSteps(COG_TAB_DISPLAY, COG_OPTION_STATS) == 0);
    CHECK(cogTrackSteps(COG_TAB_3D, COG_ROW3D_SEPARATION) == COG_SEP_STEPS);
    CHECK(cogTrackSteps(COG_TAB_3D, COG_ROW3D_CONVERGENCE) == 100);
    CHECK(cogTrackSteps(COG_TAB_3D, COG_ROW3D_PRESET) == 0);
    CHECK(cogTrackSteps(COG_TAB_COUNT, COG_ROOM_ROW_GLOW) == 0);
    CHECK(cogTrackSteps(COG_TAB_COUNT, COG_ROOM_ROW_SIZE) == 75);
    // The About tab sits just before the Room tab's number and has no tracks,
    // so none of the Room tab's lanes leak onto it
    for (int row = 0; row < COG_ROOM_ROW_COUNT; row++) {
        CHECK(cogTrackSteps(COG_TAB_ABOUT, row) == 0);
    }

    // A Room lane's steps land on whole units, so a press always moves the
    // stored value by the same number of them, from wherever a drag left it
    int lanes[2][3] = {
        { COG_ROOM_ROW_BRIGHTNESS, ROOM_BRIGHTNESS_MIN, ROOM_BRIGHTNESS_MAX },
        { COG_ROOM_ROW_LIGHT_LEVEL, ROOM_LIGHT_MIN, ROOM_LIGHT_MAX }
    };
    for (int l = 0; l < 2; l++) {
        int steps = cogTrackSteps(COG_TAB_COUNT, lanes[l][0]);
        int min = lanes[l][1];
        int max = lanes[l][2];
        CHECK((max - min) % steps == 0);
        int stride = (max - min) / steps;
        int units = min;
        int presses = 0;
        while (units < max && presses < 1000) {
            int step = cogStepIndex(lanePlace(units, min, max), steps, 1);
            int next = laneUnits((float)step / (float)steps, min, max);
            CHECK(next - units == stride);
            units = next;
            presses++;
        }
        CHECK(presses == steps);
        // From off the grid, down to the step below
        int off = min + stride * 3 + 1;
        int step = cogStepIndex(lanePlace(off, min, max), steps, -1);
        CHECK(laneUnits((float)step / (float)steps, min, max) == min + stride * 3);
    }

    // The Picture tab steps in whole units, one a press, and every row's
    // default, where its tick is, is a step
    int pictureLanes[PICTURE_VALUES][3] = {
        { PICTURE_BRIGHTNESS_MIN, PICTURE_BRIGHTNESS_MAX, PICTURE_BRIGHTNESS_DEFAULT },
        { PICTURE_CONTRAST_MIN, PICTURE_CONTRAST_MAX, PICTURE_CONTRAST_DEFAULT },
        { PICTURE_GAMMA_MIN, PICTURE_GAMMA_MAX, PICTURE_GAMMA_DEFAULT },
        { PICTURE_SATURATION_MIN, PICTURE_SATURATION_MAX, PICTURE_SATURATION_DEFAULT }
    };
    CHECK(cogTrackSteps(COG_TAB_PICTURE, PICTURE_VALUES) == 0);
    CHECK(cogTrackSteps(COG_TAB_PICTURE, -1) == 0);
    for (int row = 0; row < PICTURE_VALUES; row++) {
        int min = pictureLanes[row][0];
        int max = pictureLanes[row][1];
        int def = pictureLanes[row][2];
        int steps = cogTrackSteps(COG_TAB_PICTURE, row);
        CHECK(steps == max - min);
        int up = cogStepIndex(lanePlace(def, min, max), steps, 1);
        CHECK(laneUnits((float)up / (float)steps, min, max) == def + 1);
        int down = cogStepIndex(lanePlace(def, min, max), steps, -1);
        CHECK(laneUnits((float)down / (float)steps, min, max) == def - 1);
        CHECK(laneUnits(lanePlace(def, min, max), min, max) == def);
        CHECK(laneUnits(0.0f, min, max) == min && laneUnits(1.0f, min, max) == max);
    }

    // The tilt and roll steps clear the snap to level either side of it
    float tiltStep = 2.0f * 40.0f / (float)cogTrackSteps(COG_TAB_SCREEN, COG_SLIDER_TILT);
    float rollStep = 2.0f * 90.0f / (float)cogTrackSteps(COG_TAB_SCREEN, COG_SLIDER_ROTATE);
    CHECK(tiltStep * 3.14159265f / 180.0f > 0.0388f);
    CHECK(rollStep * 3.14159265f / 180.0f > 0.0873f);
    // And level is a step of each
    CHECK(cogTrackSteps(COG_TAB_SCREEN, COG_SLIDER_TILT) % 2 == 0);
    CHECK(cogTrackSteps(COG_TAB_SCREEN, COG_SLIDER_ROTATE) % 2 == 0);
}

// The About tab's buttons sit clear of the tab bar and the panel's edges, and
// the report sheet's fields and buttons are where a press on each lands
static void testTheReportParts(void) {
    CHECK(COG_REPORT_T > COG_TAB_BAR_B);
    CHECK(COG_REPORT_B < 1.0f && COG_REPORT_L > 0.0f && COG_REPORT_R < 1.0f);
    CHECK(cogReportButtonAt((COG_REPORT_L + COG_REPORT_R) * 0.5f,
                            (COG_REPORT_T + COG_REPORT_B) * 0.5f));
    CHECK(!cogReportButtonAt(COG_REPORT_L - 0.01f, COG_REPORT_T + 0.01f));
    CHECK(!cogReportButtonAt(0.5f, COG_REPORT_B + 0.01f));
    CHECK(!cogReportButtonAt(0.5f, 0.08f));

    // Ko-fi directly above the report button, with room between the two for
    // the line under it, and the same width
    CHECK(COG_KOFI_T > COG_TAB_BAR_B && COG_KOFI_T < COG_KOFI_B);
    CHECK(COG_KOFI_B + 0.08f < COG_REPORT_T);
    CHECK(COG_KOFI_L == COG_REPORT_L && COG_KOFI_R == COG_REPORT_R);
    float kofiV = (COG_KOFI_T + COG_KOFI_B) * 0.5f;
    float reportV = (COG_REPORT_T + COG_REPORT_B) * 0.5f;
    CHECK(cogKofiButtonAt(0.5f, kofiV));
    CHECK(!cogReportButtonAt(0.5f, kofiV));
    CHECK(!cogKofiButtonAt(0.5f, reportV));
    CHECK(!cogKofiButtonAt(0.5f, (COG_KOFI_B + COG_REPORT_T) * 0.5f));
    CHECK(!cogReportButtonAt(0.5f, (COG_KOFI_B + COG_REPORT_T) * 0.5f));
    CHECK(!cogKofiButtonAt(COG_KOFI_R + 0.01f, kofiV));

    float mid = (REPORT_FIELD_L + REPORT_FIELD_R) * 0.5f;
    CHECK(reportZone(mid, (REPORT_NOTE_T + REPORT_NOTE_B) * 0.5f) == REPORT_ZONE_NOTE);
    CHECK(reportZone(mid, (REPORT_EMAIL_T + REPORT_EMAIL_B) * 0.5f) == REPORT_ZONE_EMAIL);
    float btn = (REPORT_BTN_T + REPORT_BTN_B) * 0.5f;
    CHECK(reportZone((REPORT_CANCEL_L + REPORT_CANCEL_R) * 0.5f, btn) == REPORT_ZONE_CANCEL);
    CHECK(reportZone((REPORT_SEND_L + REPORT_SEND_R) * 0.5f, btn) == REPORT_ZONE_SEND);
    // The gap between the buttons, the words between the fields, the title
    // and the margins are nothing
    CHECK(reportZone(0.5f, btn) == REPORT_ZONE_NONE);
    CHECK(reportZone(mid, (REPORT_NOTE_B + REPORT_EMAIL_T) * 0.5f) == REPORT_ZONE_NONE);
    CHECK(reportZone(mid, (REPORT_EMAIL_B + REPORT_BTN_T) * 0.5f) == REPORT_ZONE_NONE);
    CHECK(reportZone(mid, 0.05f) == REPORT_ZONE_NONE);
    CHECK(reportZone(0.02f, (REPORT_NOTE_T + REPORT_NOTE_B) * 0.5f) == REPORT_ZONE_NONE);
    // Top to bottom without overlapping, inside the sheet
    CHECK(REPORT_NOTE_T < REPORT_NOTE_B && REPORT_NOTE_B < REPORT_EMAIL_T);
    CHECK(REPORT_EMAIL_T < REPORT_EMAIL_B && REPORT_EMAIL_B < REPORT_BTN_T);
    CHECK(REPORT_BTN_T < REPORT_BTN_B && REPORT_BTN_B < 1.0f);
    CHECK(REPORT_CANCEL_R < REPORT_SEND_L);
    // The opening is an event of its own, apart from every zone
    CHECK(REPORT_OPENED > REPORT_ZONE_SEND);
}

// The hand lock hint's two buttons are where a press on each lands, side by
// side under its words and inside the sheet
static void testTheHintButtons(void) {
    float btn = (HINT_BTN_T + HINT_BTN_B) * 0.5f;
    CHECK(handHintZone((HINT_OK_L + HINT_OK_R) * 0.5f, btn) == HINT_ZONE_OK);
    CHECK(handHintZone((HINT_NEVER_L + HINT_NEVER_R) * 0.5f, btn) == HINT_ZONE_NEVER);
    // The gap between them, the margins and the words are nothing
    CHECK(handHintZone((HINT_OK_R + HINT_NEVER_L) * 0.5f, btn) == HINT_ZONE_NONE);
    CHECK(handHintZone(HINT_OK_L - 0.01f, btn) == HINT_ZONE_NONE);
    CHECK(handHintZone(HINT_NEVER_R + 0.01f, btn) == HINT_ZONE_NONE);
    CHECK(handHintZone(0.5f, 0.3f) == HINT_ZONE_NONE);
    CHECK(handHintZone((HINT_OK_L + HINT_OK_R) * 0.5f, HINT_BTN_T - 0.01f) == HINT_ZONE_NONE);
    CHECK(handHintZone((HINT_OK_L + HINT_OK_R) * 0.5f, HINT_BTN_B + 0.01f) == HINT_ZONE_NONE);
    CHECK(HINT_OK_L > 0.0f && HINT_OK_R < HINT_NEVER_L && HINT_NEVER_R < 1.0f);
    CHECK(HINT_BTN_T > 0.5f && HINT_BTN_T < HINT_BTN_B && HINT_BTN_B < 1.0f);
    // The longer label gets the wider button
    CHECK(HINT_NEVER_R - HINT_NEVER_L > HINT_OK_R - HINT_OK_L);
}

int main(void) {
    testTheRowsFit();
    testTheReportParts();
    testTheHintButtons();
    testCornersFollowTheirArt();
    testNoCornersWhereThereAreNone();
    testTheRestOfThePicture();
    testTheStandIn();
    testTheBarUnderARoomsPicture();
    testTheBarUnderATurnedPicture();
    testTheBarOnACurvedPicture();
    testTheRowsOrder();
    testRoomCornersLookTheSameEverywhere();
    testLanes();
    testTheSizeClamp();
    testTheCornerDrag();
    testTheDepthTrack();
    testThePresetAccent();
    testTheTrackParts();
    testTheStepsLandOnTheGrid();
    testEveryTrackHasItsSteps();
    return checksDone("xr_layout");
}
