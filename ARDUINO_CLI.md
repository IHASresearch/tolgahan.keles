arduino-cli compile --fqbn esp32:esp32:esp32 .\receiver\receiver.ino ; arduino-cli compile --fqbn esp32:esp32:esp32 .\sender\sender.ino

arduino-cli upload -p COM6 --fqbn esp32:esp32:esp32 .\sender\sender.ino ; arduino-cli monitor -p COM6 -c baudrate=115200

arduino-cli upload -p COM9 --fqbn esp32:esp32:esp32 .\receiver\receiver.ino ; arduino-cli monitor -p COM9 -c baudrate=115200

arduino-cli board list