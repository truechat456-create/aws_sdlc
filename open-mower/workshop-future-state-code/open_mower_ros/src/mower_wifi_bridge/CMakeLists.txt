cmake_minimum_required(VERSION 3.0.2)
project(mower_wifi_bridge)

## mower_wifi_bridge (FR-9) is a pure-Python rospy node. We only need catkin to
## register the package, install the node script and install launch/params.

find_package(catkin REQUIRED COMPONENTS
  rospy
  mower_msgs
  std_msgs
)

catkin_package(
  CATKIN_DEPENDS rospy mower_msgs std_msgs
)

catkin_python_setup()

#############
## Install ##
#############

catkin_install_python(PROGRAMS
  nodes/mower_wifi_bridge_node
  scripts/wifi_test_client.py
  DESTINATION ${CATKIN_PACKAGE_BIN_DESTINATION}
)

install(DIRECTORY launch/
  DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}/launch
)

install(DIRECTORY params/
  DESTINATION ${CATKIN_PACKAGE_SHARE_DESTINATION}/params
)
