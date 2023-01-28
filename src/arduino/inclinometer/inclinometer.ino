
#include <Arduino.h>
#include <Arduino_LSM6DS3.h>
#include <MadgwickAHRS.h>
#include <U8g2lib.h>
#include <Geometry.h>

#ifdef U8X8_HAVE_HW_SPI
#include <SPI.h>
#endif
#ifdef U8X8_HAVE_HW_I2C
#include <Wire.h>
#endif

#include "jeep_back_frames.h"
#include "jeep_side_frames.h"

using namespace Geometry;
using namespace BLA;

// Screen constructor
U8G2_SSD1312_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, /* reset=*/ 8);

// Sensor orientation
// Default orientation (0, 0, 0) for the Arduino board is face up, USB port forward.
// const EulerAngles ea(0, 0, 0, EulerAngles::RotationFrame::Rotating, EulerAngles::RotationOrder::XYZ);
const EulerAngles ea(M_PI, 1.535889742, 0, EulerAngles::RotationFrame::Rotating, EulerAngles::RotationOrder::XYZ);

// Roll / Pitch limits that trigger the warning LED
const float rollMin = -40;
const float rollMax = 40;
const float pitchMin = -40;
const float pitchMax = 40;

// Warning LED
const int warningLed = 2;
const float warningBlinkRate = 3;

// Sensor reading and display refresh rates.
//
// The Madgwick filter must be updated regularly, matching the specified sensorRate.
// Too much deviation from the promised input rate throws off the calculation and we
// end up getting nonsense roll/pitch values.
// Updating the screen takes a ton of time; every screen update is guaranteed to
// fail to meet the requested 25Hz sensor update and introduce some errors to the filter.
// By keeping the display refresh low (5 FPS), we keep the filter error acceptably low.
// We can't really get a significantly higher FPS anyway.
const float sensorRate = 25;
const float outputRate = 5;

// Precomputed constants
unsigned const long microsPerReading = 1000000 / sensorRate;
unsigned const long microsPerOutput = 1000000 / outputRate;
unsigned const long microsPerWarningBlink = 1000000 / warningBlinkRate;
Rotation const rot = ea.to_rotation_matrix();

//

Madgwick filter;

unsigned long microsReadingPrev, microsOutputPrev;
float roll = 0.0;
float pitch = 0.0;

void setup() {
  asm(".global _printf_float");
  Serial.begin(115200);
  if (!IMU.begin()) {
    Serial.println("Failed to initialize IMU");
    while (true);
  }
  pinMode(warningLed, OUTPUT);
  u8g2.begin();
  u8g2.sendF("ca", 0xd5, 0xf0); // Adjust oscillator frequency (refresh rate) to avoid FPV camera flicker
  microsReadingPrev = microsOutputPrev = micros();
  filter.begin(sensorRate);
}

void loop() {
  unsigned long microsNow = micros();
  if (microsNow - microsReadingPrev >= microsPerReading) {
    updateAngles();
    microsReadingPrev = microsReadingPrev + microsPerReading;
  }
  if (microsNow - microsOutputPrev >= microsPerOutput) {
    updateDisplay();
    microsOutputPrev = microsOutputPrev + microsPerOutput;
  }
  updateWarningLed();
}

Matrix<3> acc = {0, 0, 0};
Matrix<3> gyro = {0, 0, 0};

void updateAngles() {
  if (IMU.accelerationAvailable() && IMU.gyroscopeAvailable()) {
    Matrix<3> imuAcc;
    Matrix<3> imuGyro;
    IMU.readAcceleration(imuAcc(0), imuAcc(1), imuAcc(2));
    IMU.readGyroscope(imuGyro(0), imuGyro(1), imuGyro(2));
    // Rotate sensor readings to match install orientation.
    acc = rot * imuAcc;
    gyro = rot * imuGyro;
  }
  filter.updateIMU(gyro(0), gyro(1), gyro(2), acc(0), acc(1), acc(2));
  roll = -filter.getRoll(); // Invert roll so the gauge works as an "artificial horizon". It's more natural.
  pitch = filter.getPitch();
}

const int numRollFrames = jeep_back_allArray_LEN;
const float rollFrameAngle = 360.f / (float)numRollFrames;
const int numPitchFrames = jeep_side_allArray_LEN;
const float pitchFrameAngle = 360.f / (float)numPitchFrames;

void updateDisplay() {
  char buf[32];
  //sprintf(buf, "roll %.0f, pitch %.0f\n", roll, pitch); Serial.print(buf);
  int rollIndex = ((int)((roll + 360.0) / rollFrameAngle + 0.5)) % numRollFrames;
  int pitchIndex = ((int)((pitch + 360.0) / pitchFrameAngle + 0.5)) % numPitchFrames;
  //sprintf(buf, "rollIndex %d, pitchIndex %d\n", rollIndex, pitchIndex); Serial.print(buf);

  u8g2.clearBuffer();
  u8g2.setBitmapMode(0);
  u8g2.drawBitmap(0, 0, 8, 64, jeep_back_allArray[rollIndex]);
  u8g2.drawBitmap(64, 0, 8, 64, jeep_side_allArray[pitchIndex]);
  u8g2.sendBuffer();
}

bool ledOnPrev = false;
unsigned long ledChangedMicros = 0;

void updateWarningLed() {
  unsigned long microsNow = micros();
  if (microsNow - ledChangedMicros < microsPerWarningBlink / 2) {
    // Blinking is throttled, each "on" or "off" state is has a minimum length (and within that period,
    // not even responding to input changes). This avoids an unpleasant flicker that may occur when we
    // just at the warning limit and get sensor noise.
    return;
  }
  bool warning = roll < rollMin || roll > rollMax || pitch < pitchMin || pitch > pitchMax;
  bool flashingOn = ((microsNow - ledChangedMicros) % microsPerWarningBlink) < microsPerWarningBlink / 1.5;
  bool ledOn = warning && flashingOn;
  if(ledOn != ledOnPrev) {
    ledChangedMicros = microsNow;
    digitalWrite(warningLed, ledOn);
    ledOnPrev = ledOn;
  }
}
