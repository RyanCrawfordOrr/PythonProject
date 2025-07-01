#!/usr/bin/env bash
set -e

BIN_DIR="$(pwd)/bin"

# install Arduino CLI if not available
if ! command -v arduino-cli &> /dev/null; then
  echo "Installing Arduino CLI..."
  BINDIR="$BIN_DIR" bash -c "$(curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh)"
fi

export PATH="$BIN_DIR:$PATH"

# create config if not exists
if [ ! -f "$HOME/.arduino15/arduino-cli.yaml" ]; then
  echo "Initializing arduino-cli config..."
  arduino-cli config init
fi

# add board index and update
arduino-cli config add board_manager.additional_urls https://downloads.arduino.cc/packages/package_renesas_index.json
arduino-cli core update-index

# install Renesas UNO R4 board core
arduino-cli core install arduino:renesas_uno@1.4.1

# install libraries
arduino-cli lib install Arducam_Mega
arduino-cli lib install ArduinoJson

# compile the main sketch to verify
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi "Arducam Livestream.ino"

# compile WiFi config sketch to verify
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi "WIFI Config/sketch_jan9a"

echo "Arduino environment setup complete."
