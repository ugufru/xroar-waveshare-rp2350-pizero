# XRoar on Waveshare RP2350 PiZero

https://www.waveshare.com/RP2350-PiZero.htm

Evaluating this board to see if it will run XRoar at least as well as the RP2350 AMOLED 1.8

We have the following successful repos in gh:
- waveshare-rp2350-usb-a
- waveshare-rp2350-touch-lcd-43b-box
- rp-2350-waveshare-touch-amoled-18
- rp-2040-waveshare-touch-lcd-169
- coco-rp2350-waveshare-touch-amoled-18 (an XRoar port that works!)

The required functionality:
- output to HDMI at 2x resolution at 30 fps+
- usb host for keyboard/joystick
- coco emulation on core 0
- video, keyboard, and sd access on core 1


