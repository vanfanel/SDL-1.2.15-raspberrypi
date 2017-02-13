# SDL-1.2.15-raspberrypi
An SDL 1.2.15 (legacy) fork that includes a better RaspberryPi native 2D API driver for optimal performance and visuals.
Uses a triple buffering mechanism and threaded vsync wait so CPU is not hold until vsync arrives before each pageflip.
This leads to optimal performance.

BUILDING
========

Run the MAC_ConfigureSDL12-rpi*.sh script corresponding to your Raspberry Pi model (1, 2, or 3), and then simply do:
make
sudo make install

It will overwrite your default debian SDL libraries, which are broken on the Pi anyway.

Thanks to exobuzz on the RaspberryPi.org forums for making me realize how bad my previous code was, and his configure.in modifications which I took from his published package!
