Activity monitor project:
Target board: (any) Generic ESP32S3 dev board
Pins: Zout = 5, Yout = 6, Xout = 7, neopixel: 48, SDA: 3, SCL: 4

Available tools: Arduino cli, tio, basic c/c++ compilations tools

Note that the ESP32S3 has 4MB flash, 2MB PSRAM

Builds ignore: ignored/Pulse_Oximeter_Plus_Google_Sheet.ino, only use as reference for code.

`make flash` compiles and flashes the project to an esp32s3 target 
`make compile` compiles the project for an esp32s3 target
