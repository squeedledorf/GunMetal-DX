Slipstream Viewer - Linux voice
===============================

Voice chat works on Linux with any headset or microphone your desktop
recognises. Slipstream uses your system's default audio devices through
PipeWire or PulseAudio; you can pick specific devices with
Preferences -> Sound & Media -> 'Voice Input/Output devices'.

Testing: the Second Life region 'Voice Echo Canyon' echoes your voice back so
you can check your microphone and levels.

Troubleshooting
---------------

* Nobody can hear you: check the microphone isn't muted and is the active
  input in your desktop's sound settings (or pavucontrol), and that
  'Speak' is enabled in the viewer.
* Others hear noise or distortion: lower the microphone gain / boost in your
  desktop's sound settings.
* No voice at all: make sure voice is enabled in Preferences -> Sound & Media
  and that you are in a voice-enabled region.

Report other problems at
https://git.slipstreamviewer.org/sky_line/Slipstream/issues
(include ~/.slipstreamviewer/logs/Slipstream.log).
