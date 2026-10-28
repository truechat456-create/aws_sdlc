# 06 - Software Architecture

> **[MODIFIED]** Future-state revision. Adds the `mower_wifi` package (FR-9.1–FR-9.6)
> to the workspace layout, node graph and launch composition. New content marked
> `[NEW]`. The baseline "Adding a New ROS Package" recipe is the construction guide
> for `mower_wifi`. See [16-WiFi-Subsystem](16-WiFi-Subsystem.md).

Baseline ROS software architecture of `open_mower_ros` (ROS1 Noetic, catkin),
focused on node structure, package layout and the interfaces a WiFi package would
attach to.

Related: [01-System-Architecture](01-System-Architecture.md),
[05-Communication-Protocols](05-Communication-Protocols.md),
[08-State-Machine-Design](08-State-Machine-Design.md),
[16-WiFi-Subsystem](16-WiFi-Subsystem.md).

## Workspace Layout

The catkin workspace lives under `src/`:

```
src/
  open_mower/          # top-level launch + params (entry point)
  mower_logic/         # behavior state machine, monitoring
  mower_comms_v1/      # serial (COBS) bridge to firmware
  mower_comms_v2/      # xbot_framework service-bus bridge to firmware
  mower_map/           # map storage + service
  mower_msgs/          # shared messages and services
  mower_utils/         # helpers (pose converter, planner test)
  mower_simulation/    # simulated robot + services
  mower_wifi/          # [NEW] WiFi remote start/stop endpoint (FR-9)
  lib/                 # vendored libraries (see below)
services/              # xtech "definitions-open-mower" (v2 service defs, submodule)
```

> `[NEW]` `mower_wifi` is a new first-class package in `src/`. It depends only on
> `roscpp` (or `rospy`) and `mower_msgs`; it introduces no new message package. It is
> built by catkin automatically once present under `src/`.

Vendored libraries in `src/lib/` include `xbot_framework`, `xbot_msgs`,
`xbot_positioning`, `xbot_monitoring`, `xbot_mqtt`, `xbot_remote`, `xesc_ros`,
`ftc_local_planner`, `slic3r_coverage_planner`, `xbot_driver_gps`, `ntrip_client`
and `json`.

## Node Structure (runtime)

Launched from `open_mower/launch/open_mower.launch`, which composes:

```
open_mower.launch
  include/_comms.launch        -> ll / mower_comms_v2  (+ gps, ntrip)
  include/_move_base.launch    -> move_base_flex + planners
  include/_localization.launch -> xbot_positioning (EKF)
  include/_teleop.launch       -> joystick teleop
  include/_record.launch       -> rosbag recording
  map_service       (mower_map)
  mower_logic       (behavior state machine)   <-- core controller
  slic3r_coverage_planner
  twist_mux         -> remaps cmd_vel_out to /ll/cmd_vel
  xbot_monitoring   -> app/monitoring bridge (remote_cmd_vel -> /joy_vel)
  heatmap_generator (optional)
  xbot_remote       -> /joy_vel
  monitoring        (mower_logic/monitoring)
  mower_wifi        [NEW] -> WiFi remote endpoint (FR-9)
  include/_params.launch
```

Most nodes declare `respawn="true" respawn_delay="10"` for resilience (NFR-2).

> `[NEW]` `mower_wifi` is added to `open_mower.launch` (directly, or via a new
> `include/_wifi.launch` included from it) with `respawn="true" respawn_delay="10"`.
> It is gated by an env/arg (e.g. `unless="$(optenv OM_NO_WIFI False)"`) so builds
> that do not want the remote endpoint can disable it. Its port and token parameters
> load from `params/` (see [16-WiFi-Subsystem](16-WiFi-Subsystem.md)).

## Primary Data/Control Flow

```
firmware --(mower_comms_*)--> /ll/* topics
     /ll/imu + /ll/position/gps --> xbot_positioning --> /xbot_positioning/xb_pose
     /ll/mower_status, /ll/power, /ll/emergency --> mower_logic (StateSubscriber)
mower_logic --> move_base_flex / ftc_local_planner --> cmd_vel
cmd_vel (+ /joy_vel teleop) --> twist_mux --> /ll/cmd_vel --> mower_comms_* --> firmware
mower_logic --> /mower_logic/current_state (HighLevelStatus, latched)
app <-- xbot_monitoring / xbot_mqtt / rosbridge <--> mower_logic command+status
[NEW] remote client <--WiFi(token)--> mower_wifi
          mower_wifi --> mower_service/high_level_control (START/HOME/RESET)
          mower_wifi --> ll/_service/emergency (EMERGENCY, < 1 s, FR-9.4)
          mower_wifi <-- /mower_logic/current_state (1 Hz status, FR-9.6)
```

`mower_logic` consumes its inputs through a small `StateSubscriber<T>` wrapper, e.g.:

```cpp
StateSubscriber<mower_msgs::Emergency> emergency_state_subscriber{"/ll/emergency"};
StateSubscriber<mower_msgs::Status>    status_state_subscriber{"/ll/mower_status"};
StateSubscriber<xbot_msgs::AbsolutePose> pose_state_subscriber{"/xbot_positioning/xb_pose"};
```

## Message / Service Interfaces (`mower_msgs`)

Messages: `Status`, `Emergency`, `Power`, `Bms`, `ESCStatus`, `ImuRaw`,
`HighLevelStatus`, `Perimeter`, `DockingSensor`.
Services: `HighLevelControlSrv`, `MowerControlSrv`, `EmergencyStopSrv`,
`GPSControlSrv`, `PerimeterControlSrv`.

The main command entry point for external features (including a future WiFi/app
command path) is:

- Service `mower_service/high_level_control` (`HighLevelControlSrv`) - start, home,
  S1, S2, reset-emergency, delete-maps (see [08](08-State-Machine-Design.md)).
- Topic `xbot/action` (`std_msgs/String`) - string-keyed actions.
- Topic `/mower_logic/current_state` (`HighLevelStatus`, latched) - status to report.

## Adding a New ROS Package (the WiFi package)

To add a package (e.g. `mower_wifi`) to the workspace:

1. **Create the package directory** under `src/mower_wifi/` with:
   - `package.xml` (format 2) declaring `<depend>` on `roscpp`, `mower_msgs`, and
     any `xbot_*` libs needed. Mirror an existing one such as `mower_comms_v2`:
     ```xml
     <package format="2">
       <name>mower_wifi</name>
       <buildtool_depend>catkin</buildtool_depend>
       <depend>roscpp</depend>
       <depend>mower_msgs</depend>
       <exec_depend>roscpp</exec_depend>
     </package>
     ```
   - `CMakeLists.txt` with `find_package(catkin REQUIRED COMPONENTS ...)`,
     `catkin_package(...)`, and `add_executable` + `target_link_libraries` for the
     node, following the existing package CMake files.
   - `src/` for the node, optional `launch/` and `params/`.
2. **Wire it into launch.** Add a `<node pkg="mower_wifi" .../>` entry to
   `open_mower.launch` (or a new `include/_wifi.launch` included from it), with
   `respawn="true" respawn_delay="10"` per convention.
3. **Integrate via existing interfaces.** Call `mower_service/high_level_control`
   for commands and subscribe `/mower_logic/current_state` for status - do not bypass
   `mower_logic` or the safety chain. If WiFi also needs a firmware channel, extend
   `mower_comms_v2` instead (see [05](05-Communication-Protocols.md)).
4. **Per-robot config.** Place tunables under
   `open_mower/params/hardware_specific/<model>/` and load them in the launch file,
   matching the existing comms/gps param split.
5. **Build.** `catkin build` (or `catkin_make`) picks up the new package
   automatically once it is under `src/`.
