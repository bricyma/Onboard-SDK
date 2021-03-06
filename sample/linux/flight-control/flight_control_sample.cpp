/*! @file flight_control_sample.cpp
 *  @version 3.3
 *  @date Jun 05 2017
 *
 *  @brief
 *  Flight Control API usage in a Linux environment.
 *  Provides a number of helpful additions to core API calls,
 *  especially for position control, attitude control, takeoff,
 *  landing.
 *
 *  @copyright
 *  2016-17 DJI. All rights reserved.
 * */

#include "flight_control_sample.hpp"

using namespace DJI::OSDK;
using namespace DJI::OSDK::Telemetry;

/*! main
 *
 */
int
main(int argc, char** argv)
{
  // Initialize variables
  int functionTimeout = 1;

  // Setup OSDK.
  Vehicle* vehicle = setupOSDK(argc, argv);
  if (vehicle == NULL)
  {
    std::cout << "Vehicle not initialized, exiting.\n";
    return -1;
  }

  // Obtain Control Authority
  vehicle->obtainCtrlAuthority(functionTimeout);

  // Display interactive prompt
  std::cout
    << "| Available commands:                                            |"
    << std::endl;
  std::cout
    << "| [a] Monitored Takeoff + Landing                                |"
    << std::endl;
  std::cout
    << "| [b] Monitored Takeoff + Position Control + Landing             |"
    << std::endl;
  char inputChar;
  std::cin >> inputChar;

  switch (inputChar)
  {
    case 'a':
      monitoredTakeoff(vehicle);
      monitoredLanding(vehicle);
      break;
    case 'b':
      monitoredTakeoff(vehicle);
      moveByPositionOffset(vehicle, 0, 6, 6, 30);
      moveByPositionOffset(vehicle, 6, 0, -3, -30);
      moveByPositionOffset(vehicle, -6, -6, 0, 0);
      monitoredLanding(vehicle);
      break;
    default:
      break;
  }

  delete (vehicle);
  return 0;
}

/*! Monitored Takeoff (Blocking API call). Return status as well as ack.
    This version of takeoff makes sure your aircraft actually took off
    and only returns when takeoff is complete.
    Use unless you want to do other stuff during takeoff - this will block
    the main thread.
!*/
bool
monitoredTakeoff(Vehicle* vehicle, int timeout)
{
  //@todo: remove this once the getErrorCode function signature changes
  char func[50];

  // Telemetry: Verify the subscription
  ACK::ErrorCode subscribeStatus;
  subscribeStatus = vehicle->subscribe->verify(timeout);
  if (ACK::getError(subscribeStatus) != ACK::SUCCESS)
  {
    ACK::getErrorCodeMessage(subscribeStatus, func);
    return false;
  }

  // Telemetry: Subscribe to flight status and mode at freq 10 Hz
  int       pkgIndex        = 0;
  int       freq            = 10;
  TopicName topicList10Hz[] = { TOPIC_STATUS_FLIGHT, TOPIC_STATUS_DISPLAYMODE };
  int       numTopic        = sizeof(topicList10Hz) / sizeof(topicList10Hz[0]);
  bool      enableTimestamp = false;

  bool pkgStatus = vehicle->subscribe->initPackageFromTopicList(
    pkgIndex, numTopic, topicList10Hz, enableTimestamp, freq);
  if (!(pkgStatus))
  {
    return pkgStatus;
  }
  subscribeStatus = vehicle->subscribe->startPackage(pkgIndex, timeout);
  if (ACK::getError(subscribeStatus) != ACK::SUCCESS)
  {
    ACK::getErrorCodeMessage(subscribeStatus, func);
    // Cleanup before return
    vehicle->subscribe->removePackage(pkgIndex, timeout);
    return false;
  }

  // Start takeoff
  ACK::ErrorCode takeoffStatus = vehicle->control->takeoff(timeout);
  if (ACK::getError(takeoffStatus) != ACK::SUCCESS)
  {
    ACK::getErrorCodeMessage(takeoffStatus, func);
    return false;
  }

  // First check: Motors started
  int motorsNotStarted = 0;
  int timeoutCycles    = 20;
  while (vehicle->subscribe->getValue<TOPIC_STATUS_FLIGHT>() !=
           OpenProtocol::ErrorCode::CommonACK::FlightStatus::ON_GROUND &&
         vehicle->subscribe->getValue<TOPIC_STATUS_DISPLAYMODE>() !=
           VehicleStatus::MODE_ENGINE_START &&
         motorsNotStarted < timeoutCycles)
  {
    motorsNotStarted++;
    usleep(100000);
  }

  if (motorsNotStarted == timeoutCycles)
  {
    std::cout << "Takeoff failed. Motors are not spinning." << std::endl;
    // Cleanup before return
    vehicle->subscribe->removePackage(0, timeout);
    return false;
  }
  else
  {
    std::cout << "Motors spinning...\n";
  }

  // Second check: In air
  int stillOnGround = 0;
  timeoutCycles     = 110;
  while (vehicle->subscribe->getValue<TOPIC_STATUS_FLIGHT>() !=
           OpenProtocol::ErrorCode::CommonACK::FlightStatus::IN_AIR &&
         (vehicle->subscribe->getValue<TOPIC_STATUS_DISPLAYMODE>() !=
            VehicleStatus::MODE_ASSISTED_TAKEOFF ||
          vehicle->subscribe->getValue<TOPIC_STATUS_DISPLAYMODE>() !=
            VehicleStatus::MODE_AUTO_TAKEOFF) &&
         stillOnGround < timeoutCycles)
  {
    stillOnGround++;
    usleep(100000);
  }

  if (stillOnGround == timeoutCycles)
  {
    std::cout << "Takeoff failed. Aircraft is still on the ground, but the "
                 "motors are spinning."
              << std::endl;
    // Cleanup before return
    vehicle->subscribe->removePackage(0, timeout);
    return false;
  }
  else
  {
    std::cout << "Ascending...\n";
  }

  // Final check: Finished takeoff
  while (vehicle->subscribe->getValue<TOPIC_STATUS_DISPLAYMODE>() ==
           VehicleStatus::MODE_ASSISTED_TAKEOFF ||
         vehicle->subscribe->getValue<TOPIC_STATUS_DISPLAYMODE>() ==
           VehicleStatus::MODE_AUTO_TAKEOFF)
  {
    sleep(1);
  }

  if (vehicle->subscribe->getValue<TOPIC_STATUS_DISPLAYMODE>() !=
        VehicleStatus::MODE_P_GPS ||
      vehicle->subscribe->getValue<TOPIC_STATUS_DISPLAYMODE>() !=
        VehicleStatus::MODE_ATTITUDE)
  {
    std::cout << "Successful takeoff!\n";
  }
  else
  {
    std::cout << "Takeoff finished, but the aircraft is in an unexpected mode. "
                 "Please connect DJI GO.\n";
    vehicle->subscribe->removePackage(0, timeout);
    return false;
  }

  // Cleanup before return
  ACK::ErrorCode ack = vehicle->subscribe->removePackage(pkgIndex, timeout);
  if (ACK::getError(ack))
  {
    std::cout << "Error unsubscribing; please restart the drone/FC to get back "
                 "to a clean state.\n";
  }

  return true;
}

/*! Position Control. Allows you to set an offset from your current
    location. The aircraft will move to that position and stay there.
    Typical use would be as a building block in an outer loop that does not
    require many fast changes, perhaps a few-waypoint trajectory. For smoother
    transition and response you should convert your trajectory to attitude
    setpoints and use attitude control or convert to velocity setpoints
    and use velocity control.
!*/
int
moveByPositionOffset(Vehicle* vehicle, float xOffsetDesired,
                     float yOffsetDesired, float zOffsetDesired,
                     float yawDesired, float posThresholdInM,
                     float yawThresholdInDeg)
{
  // Set timeout: this timeout is the time you allow the drone to take to finish
  // the
  // mission
  int responseTimeout              = 1;
  int timeoutInMilSec              = 10000;
  int controlFreqInHz              = 50; // Hz
  int cycleTimeInMs                = 1000 / controlFreqInHz;
  int outOfControlBoundsTimeLimit  = 10 * cycleTimeInMs; // 10 cycles
  int withinControlBoundsTimeReqmt = 50 * cycleTimeInMs; // 50 cycles

  //@todo: remove this once the getErrorCode function signature changes
  char func[50];

  // Telemetry: Verify the subscription
  ACK::ErrorCode subscribeStatus;
  subscribeStatus = vehicle->subscribe->verify(responseTimeout);
  if (ACK::getError(subscribeStatus) != ACK::SUCCESS)
  {
    ACK::getErrorCodeMessage(subscribeStatus, func);
    return false;
  }

  // Telemetry: Subscribe to quaternion, fused lat/lon and altitude at freq 50
  // Hz
  int       pkgIndex        = 0;
  int       freq            = 50;
  TopicName topicList50Hz[] = { TOPIC_QUATERNION, TOPIC_GPS_FUSED };
  int       numTopic        = sizeof(topicList50Hz) / sizeof(topicList50Hz[0]);
  bool      enableTimestamp = false;

  bool pkgStatus = vehicle->subscribe->initPackageFromTopicList(
    pkgIndex, numTopic, topicList50Hz, enableTimestamp, freq);
  if (!(pkgStatus))
  {
    return pkgStatus;
  }
  subscribeStatus = vehicle->subscribe->startPackage(pkgIndex, responseTimeout);
  if (ACK::getError(subscribeStatus) != ACK::SUCCESS)
  {
    ACK::getErrorCodeMessage(subscribeStatus, func);
    // Cleanup before return
    vehicle->subscribe->removePackage(pkgIndex, responseTimeout);
    return false;
  }

  // Wait for data to come in
  sleep(1);

  // Get data
  Telemetry::TypeMap<TOPIC_GPS_FUSED>::type currentGPS =
    vehicle->subscribe->getValue<TOPIC_GPS_FUSED>();
  Telemetry::TypeMap<TOPIC_GPS_FUSED>::type originGPS = currentGPS;

  // Convert position offset from first position to local coordinates
  Telemetry::Vector3f localOffset;
  localOffsetFromGpsOffset(localOffset, currentGPS, originGPS);

  // Get initial offset. We will update this in a loop later.
  double xOffsetRemaining = xOffsetDesired - localOffset.x;
  double yOffsetRemaining = yOffsetDesired - localOffset.y;
  double zOffsetRemaining = zOffsetDesired - (-localOffset.z);

  // Conversions
  double yawDesiredRad     = DEG2RAD * yawDesired;
  double yawThresholdInRad = DEG2RAD * yawThresholdInDeg;

  //! Get Euler angle
  Telemetry::TypeMap<TOPIC_QUATERNION>::type q =
    vehicle->subscribe->getValue<TOPIC_QUATERNION>();
  double yawInRad = toEulerAngle(q).z / DEG2RAD;

  int   elapsedTimeInMs     = 0;
  int   withinBoundsCounter = 0;
  int   outOfBounds         = 0;
  int   brakeCounter        = 0;
  int   speedFactor         = 2;
  float xCmd, yCmd, zCmd;
  // There is a deadband in position control
  // the z cmd is absolute height
  // while x and y are in relative
  float zDeadband = 0.12;

  /*! Calculate the inputs to send the position controller. We implement basic
   *  receding setpoint position control and the setpoint is always 1 m away
   *  from the current position - until we get within a threshold of the goal.
   *  From that point on, we send the remaining distance as the setpoint.
   */
  if (xOffsetDesired > 0)
    xCmd = (xOffsetDesired < speedFactor) ? xOffsetDesired : speedFactor;
  else if (xOffsetDesired < 0)
    xCmd =
      (xOffsetDesired > -1 * speedFactor) ? xOffsetDesired : -1 * speedFactor;
  else
    xCmd = 0;

  if (yOffsetDesired > 0)
    yCmd = (yOffsetDesired < speedFactor) ? yOffsetDesired : speedFactor;
  else if (yOffsetDesired < 0)
    yCmd =
      (yOffsetDesired > -1 * speedFactor) ? yOffsetDesired : -1 * speedFactor;
  else
    yCmd = 0;

  zCmd = currentGPS.altitude + zOffsetDesired;

  //! Main closed-loop receding setpoint position control
  while (elapsedTimeInMs < timeoutInMilSec)
  {

    vehicle->control->positionAndYawCtrl(xCmd, yCmd, zCmd,
                                         yawDesiredRad / DEG2RAD);

    usleep(cycleTimeInMs * 1000);
    elapsedTimeInMs += cycleTimeInMs;

    //! Get current position in required coordinates and units
    q          = vehicle->subscribe->getValue<TOPIC_QUATERNION>();
    yawInRad   = toEulerAngle(q).z;
    currentGPS = vehicle->subscribe->getValue<TOPIC_GPS_FUSED>();
    localOffsetFromGpsOffset(localOffset, currentGPS, originGPS);

    //! See how much farther we have to go
    xOffsetRemaining = xOffsetDesired - localOffset.x;
    yOffsetRemaining = yOffsetDesired - localOffset.y;
    zOffsetRemaining = zOffsetDesired - (-localOffset.z);

    //! See if we need to modify the setpoint
    if (std::abs(xOffsetRemaining) < speedFactor)
      xCmd = xOffsetRemaining;
    if (std::abs(yOffsetRemaining) < speedFactor)
      yCmd = yOffsetRemaining;

    if (std::abs(xOffsetRemaining) < posThresholdInM &&
        std::abs(yOffsetRemaining) < posThresholdInM &&
        std::abs(zOffsetRemaining) < zDeadband &&
        std::abs(yawInRad - yawDesiredRad) < yawThresholdInRad)
    {
      //! 1. We are within bounds; start incrementing our in-bound counter
      withinBoundsCounter += cycleTimeInMs;
    }
    else
    {
      if (withinBoundsCounter != 0)
      {
        //! 2. Start incrementing an out-of-bounds counter
        outOfBounds += cycleTimeInMs;
      }
    }
    //! 3. Reset withinBoundsCounter if necessary
    if (outOfBounds > outOfControlBoundsTimeLimit)
    {
      withinBoundsCounter = 0;
      outOfBounds         = 0;
    }
    //! 4. If within bounds, set flag and break
    if (withinBoundsCounter >= withinControlBoundsTimeReqmt)
    {
      break;
    }
  }

  //! Set velocity to zero, to prevent any residual velocity from position
  //! command

  while (brakeCounter < withinControlBoundsTimeReqmt)
  {
    vehicle->control->emergencyBrake();
    usleep(cycleTimeInMs);
    brakeCounter += cycleTimeInMs;
  }

  if (elapsedTimeInMs >= timeoutInMilSec)
  {
    std::cout << "Task timeout!\n";
    ACK::ErrorCode ack =
      vehicle->subscribe->removePackage(pkgIndex, responseTimeout);
    if (ACK::getError(ack))
    {
      std::cout << "Error unsubscribing; please restart the drone/FC to get "
                   "back to a clean state.\n";
    }
    return ACK::FAIL;
  }

  ACK::ErrorCode ack =
    vehicle->subscribe->removePackage(pkgIndex, responseTimeout);
  if (ACK::getError(ack))
  {
    std::cout << "Error unsubscribing; please restart the drone/FC to get back "
                 "to a clean state.\n";
  }
  return ACK::SUCCESS;
}

/*! Monitored Takeoff (Blocking API call). Return status as well as ack.
    This version of takeoff makes sure your aircraft actually took off
    and only returns when takeoff is complete.
    Use unless you want to do other stuff during takeoff - this will block
    the main thread.
!*/
bool
monitoredLanding(Vehicle* vehicle, int timeout)
{
  //@todo: remove this once the getErrorCode function signature changes
  char func[50];

  // Telemetry: Verify the subscription
  ACK::ErrorCode subscribeStatus;
  subscribeStatus = vehicle->subscribe->verify(timeout);
  if (ACK::getError(subscribeStatus) != ACK::SUCCESS)
  {
    ACK::getErrorCodeMessage(subscribeStatus, func);
    return false;
  }

  // Telemetry: Subscribe to flight status and mode at freq 10 Hz
  int       pkgIndex        = 0;
  int       freq            = 10;
  TopicName topicList10Hz[] = { TOPIC_STATUS_FLIGHT, TOPIC_STATUS_DISPLAYMODE };
  int       numTopic        = sizeof(topicList10Hz) / sizeof(topicList10Hz[0]);
  bool      enableTimestamp = false;

  bool pkgStatus = vehicle->subscribe->initPackageFromTopicList(
    pkgIndex, numTopic, topicList10Hz, enableTimestamp, freq);
  if (!(pkgStatus))
  {
    return pkgStatus;
  }
  subscribeStatus = vehicle->subscribe->startPackage(pkgIndex, timeout);
  if (ACK::getError(subscribeStatus) != ACK::SUCCESS)
  {
    ACK::getErrorCodeMessage(subscribeStatus, func);
    // Cleanup before return
    vehicle->subscribe->removePackage(pkgIndex, timeout);
    return false;
  }

  // Start landing
  ACK::ErrorCode landingStatus = vehicle->control->land(timeout);
  if (ACK::getError(landingStatus) != ACK::SUCCESS)
  {
    ACK::getErrorCodeMessage(landingStatus, func);
    return false;
  }

  // First check: Landing started
  int landingNotStarted = 0;
  int timeoutCycles     = 20;
  while (vehicle->subscribe->getValue<TOPIC_STATUS_DISPLAYMODE>() !=
           VehicleStatus::MODE_AUTO_LANDING &&
         landingNotStarted < timeoutCycles)
  {
    landingNotStarted++;
    usleep(100000);
  }

  if (landingNotStarted == timeoutCycles)
  {
    std::cout << "Landing failed. Aircraft is still in the air." << std::endl;
    // Cleanup before return
    ACK::ErrorCode ack = vehicle->subscribe->removePackage(pkgIndex, timeout);
    if (ACK::getError(ack))
    {
      std::cout << "Error unsubscribing; please restart the drone/FC to get "
                   "back to a clean state.\n";
    }
    return false;
  }
  else
  {
    std::cout << "Landing...\n";
  }

  // Second check: Finished landing
  while (vehicle->subscribe->getValue<TOPIC_STATUS_DISPLAYMODE>() ==
           VehicleStatus::MODE_AUTO_LANDING &&
         vehicle->subscribe->getValue<TOPIC_STATUS_FLIGHT>() ==
           OpenProtocol::ErrorCode::CommonACK::FlightStatus::IN_AIR)
  {
    sleep(1);
  }

  if (vehicle->subscribe->getValue<TOPIC_STATUS_DISPLAYMODE>() !=
        VehicleStatus::MODE_P_GPS ||
      vehicle->subscribe->getValue<TOPIC_STATUS_DISPLAYMODE>() !=
        VehicleStatus::MODE_ATTITUDE)
  {
    std::cout << "Successful landing!\n";
  }
  else
  {
    std::cout << "Landing finished, but the aircraft is in an unexpected mode. "
                 "Please connect DJI GO.\n";
    ACK::ErrorCode ack = vehicle->subscribe->removePackage(pkgIndex, timeout);
    if (ACK::getError(ack))
    {
      std::cout << "Error unsubscribing; please restart the drone/FC to get "
                   "back to a clean state.\n";
    }
    return false;
  }

  // Cleanup before return
  ACK::ErrorCode ack = vehicle->subscribe->removePackage(pkgIndex, timeout);
  if (ACK::getError(ack))
  {
    std::cout << "Error unsubscribing; please restart the drone/FC to get back "
                 "to a clean state.\n";
  }
  return true;
}

// Helper Functions

/*! Very simple calculation of local NED offset between two pairs of GPS
/coordinates.
    Accurate when distances are small.
!*/
void
localOffsetFromGpsOffset(Telemetry::Vector3f& deltaNed,
                         Telemetry::GPSFused& target,
                         Telemetry::GPSFused& origin)
{
  double deltaLon = target.longitude - origin.longitude;
  double deltaLat = target.latitude - origin.latitude;
  deltaNed.x      = deltaLat * C_EARTH;
  deltaNed.y      = deltaLon * C_EARTH * cos(target.latitude);
  deltaNed.z      = target.altitude - origin.altitude;
}

Telemetry::Vector3f
toEulerAngle(Telemetry::TypeMap<TOPIC_QUATERNION>::type& quaternionData)
{
  Telemetry::Vector3f ans;

  double q2sqr = quaternionData.q2 * quaternionData.q2;
  double t0    = -2.0 * (q2sqr + quaternionData.q3 * quaternionData.q3) + 1.0;
  double t1    = +2.0 * (quaternionData.q1 * quaternionData.q2 +
                      quaternionData.q0 * quaternionData.q3);
  double t2 = -2.0 * (quaternionData.q1 * quaternionData.q3 -
                      quaternionData.q0 * quaternionData.q2);
  double t3 = +2.0 * (quaternionData.q2 * quaternionData.q3 +
                      quaternionData.q0 * quaternionData.q1);
  double t4 = -2.0 * (quaternionData.q1 * quaternionData.q1 + q2sqr) + 1.0;

  t2 = (t2 > 1.0) ? 1.0 : t2;
  t2 = (t2 < -1.0) ? -1.0 : t2;

  ans.x = asin(t2);
  ans.y = atan2(t3, t4);
  ans.z = atan2(t1, t0);

  return ans;
}
