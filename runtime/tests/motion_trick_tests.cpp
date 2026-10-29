#include "motion_trick_detector.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using MotionTrick::Direction;
using MotionTrick::MotionSample;
using MotionTrick::ShakeDetector;

namespace {

int g_failures = 0;
constexpr uint64_t kFrameUs = 16'667;

void Check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("  FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

// Built from bits so -ffast-math cannot assume them away.
float FromBits(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

MotionSample Gyro(float pitch, float yaw, float roll) {
    MotionSample sample;
    sample.hasGyro = true;
    sample.gyro[0] = pitch;
    sample.gyro[1] = yaw;
    sample.gyro[2] = roll;
    sample.hasAccel = true;
    sample.accel[1] = 9.80665f;
    return sample;
}

MotionSample AtRest() {
    return Gyro(0.0f, 0.0f, 0.0f);
}

struct Run {
    ShakeDetector detector;
    uint64_t now = 0;
    std::vector<Direction> fired;

    void Feed(const MotionSample& sample, int frames = 1) {
        for (int i = 0; i < frames; ++i) {
            const Direction direction = detector.Update(sample, now);
            if (direction != Direction::None) fired.push_back(direction);
            now += kFrameUs;
        }
    }

    // Ramp up to the peak and back down over a few frames, like a real flick.
    void Flick(const MotionSample& peak) {
        for (const float scale : {0.3f, 0.7f, 1.0f, 0.6f, 0.2f}) {
            MotionSample sample = peak;
            for (float& value : sample.gyro) value *= scale;
            Feed(sample);
        }
    }
};

void TestRestNeverFires() {
    Run run;
    run.Feed(AtRest(), 600);
    Check(run.fired.empty(), "controller at rest fired a trick");
}

void TestSteeringWobbleNeverFires() {
    Run run;
    for (int i = 0; i < 600; ++i) {
        run.Feed(Gyro(1.5f * std::sin(i * 0.2f), 2.0f * std::sin(i * 0.13f), 2.5f * std::cos(i * 0.17f)));
    }
    Check(run.fired.empty(), "steering wobble fired a trick");
}

void TestOneFlickOneTrick() {
    Run run;
    run.Flick(Gyro(10.0f, 0.0f, 0.0f));
    run.Feed(AtRest(), 60);
    Check(run.fired.size() == 1, "one flick did not give exactly one trick");
    Check(!run.fired.empty() && run.fired[0] == Direction::Up, "upward flick was not Up");
}

void TestReturnStrokeIsIgnored() {
    Run run;
    run.Flick(Gyro(10.0f, 0.0f, 0.0f));
    run.Flick(Gyro(-9.0f, 0.0f, 0.0f));
    run.Feed(AtRest(), 60);
    Check(run.fired.size() == 1, "bringing the controller back gave a second trick");
}

void TestSeparateFlicksBothFire() {
    Run run;
    run.Flick(Gyro(10.0f, 0.0f, 0.0f));
    run.Feed(AtRest(), 40);
    run.Flick(Gyro(10.0f, 0.0f, 0.0f));
    run.Feed(AtRest(), 40);
    Check(run.fired.size() == 2, "two separate flicks did not give two tricks");
}

void TestSustainedShakeFiresOnce() {
    Run run;
    for (int i = 0; i < 120; ++i) {
        run.Feed(Gyro(i % 2 == 0 ? 12.0f : -12.0f, 0.0f, 0.0f));
    }
    Check(run.fired.size() == 1, "continuous shaking fired more than once");
}

void TestDirections() {
    const struct {
        MotionSample peak;
        Direction expected;
        const char* name;
    } cases[] = {
        {Gyro(10.0f, 0.0f, 0.0f), Direction::Up, "pitch up"},
        {Gyro(-10.0f, 0.0f, 0.0f), Direction::Down, "pitch down"},
        {Gyro(1.0f, 0.0f, 10.0f), Direction::Left, "roll left"},
        {Gyro(1.0f, 0.0f, -10.0f), Direction::Right, "roll right"},
        {Gyro(0.0f, 10.0f, 2.0f), Direction::Left, "yaw left"},
        {Gyro(0.0f, -10.0f, -2.0f), Direction::Right, "yaw right"},
    };
    for (const auto& c : cases) {
        Run run;
        run.Flick(c.peak);
        Check(run.fired.size() == 1 && run.fired[0] == c.expected, std::string("wrong direction for ") + c.name);
    }
}

void TestSensitivity() {
    Run low;
    low.detector.SetSensitivity(1);
    low.Flick(Gyro(6.0f, 0.0f, 0.0f));
    Check(low.fired.empty(), "sensitivity 1 fired on a small flick");

    Run high;
    high.detector.SetSensitivity(5);
    high.Flick(Gyro(6.0f, 0.0f, 0.0f));
    Check(high.fired.size() == 1, "sensitivity 5 missed a small flick");

    Run clamped;
    clamped.detector.SetSensitivity(99);
    clamped.Flick(Gyro(6.0f, 0.0f, 0.0f));
    Check(clamped.fired.size() == 1, "out of range sensitivity was not clamped");
}

void TestAccelOnlyController() {
    Run run;
    MotionSample rest;
    rest.hasAccel = true;
    rest.accel[1] = 9.80665f;
    run.Feed(rest, 120);
    Check(run.fired.empty(), "gravity alone fired a trick");

    MotionSample jolt = rest;
    jolt.accel[1] = 35.0f;
    run.Feed(jolt);
    run.Feed(rest, 60);
    Check(run.fired.size() == 1 && run.fired[0] == Direction::Up, "accelerometer jolt did not give Up");
}

void TestGyroOnlyController() {
    Run run;
    MotionSample flick = Gyro(10.0f, 0.0f, 0.0f);
    flick.hasAccel = false;
    run.Flick(flick);
    run.Feed(AtRest(), 60);
    Check(run.fired.size() == 1 && run.fired[0] == Direction::Up, "gyro-only flick did not give Up");
}

void TestGyroPadIgnoresAccel() {
    Run bump;
    MotionSample jolt = AtRest();
    jolt.accel[1] = 40.0f;
    bump.Feed(jolt, 3);
    bump.Feed(AtRest(), 60);
    Check(bump.fired.empty(), "an accelerometer bump on a gyro pad fired a trick");

    Run flick;
    MotionSample sideways = Gyro(0.0f, 10.0f, 0.0f);
    sideways.accel[1] = 40.0f;
    flick.Feed(sideways);
    Check(flick.fired.size() == 1 && flick.fired[0] == Direction::Left,
          "a jolt changed the direction of a sideways flick");
}

void TestNoSensorsNeverFires() {
    Run run;
    MotionSample none;
    none.gyro[0] = 50.0f;
    none.accel[1] = 80.0f;
    run.Feed(none, 60);
    Check(run.fired.empty(), "a sample without sensors fired a trick");
}

void TestBadDataIgnored() {
    Run run;
    const float nan = FromBits(0x7fc00000u);
    const float inf = FromBits(0x7f800000u);
    run.Feed(Gyro(nan, 0.0f, 0.0f), 10);
    run.Feed(Gyro(inf, 0.0f, 0.0f), 10);
    Check(run.fired.empty(), "NaN or infinite sensor data fired a trick");
    run.Flick(Gyro(10.0f, 0.0f, 0.0f));
    Check(run.fired.size() == 1, "detector did not recover after NaN data");
}

} // namespace

int main() {
    TestRestNeverFires();
    TestSteeringWobbleNeverFires();
    TestOneFlickOneTrick();
    TestReturnStrokeIsIgnored();
    TestSeparateFlicksBothFire();
    TestSustainedShakeFiresOnce();
    TestDirections();
    TestSensitivity();
    TestAccelOnlyController();
    TestGyroOnlyController();
    TestGyroPadIgnoresAccel();
    TestNoSensorsNeverFires();
    TestBadDataIgnored();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("motion trick tests passed\n");
    return 0;
}
