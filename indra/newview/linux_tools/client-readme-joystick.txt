Slipstream Viewer - Linux joysticks and 3D mice
===============================================

Slipstream can use a joystick, gamepad or 3Dconnexion SpaceMouse /
SpaceNavigator to move your avatar and camera.

Setup
-----

1. Plug the device in before starting Slipstream so it is detected.
2. Open Preferences -> Move & View and click 'Other Devices' to enable the
   device and adjust axes and sensitivity. 3D mouse users can start from the
   'SpaceNavigator Defaults' button.

Your user needs permission to read the device's /dev/input/event* node. Most
distributions grant this to the logged-in user automatically; if not, add a
udev rule for the device or add your user to the 'input' group.

Known problems
--------------

* If your desktop treats the device as a mouse (it moves your pointer when
  you plug it in), the viewer may detect it but not be able to use it.
* Only the first detected joystick or 3D mouse is used.

Report other problems at
https://git.slipstreamviewer.org/sky_line/Slipstream/issues
