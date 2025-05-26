#!/bin/bash
# -----------------------------------------------------------------------------
# ota.sh – ESP8266 OTA Upload Script
#
# (c) 2025 Tobias Stöger, @tstoegi
# Released under the MIT License
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in all
# copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.
# -----------------------------------------------------------------------------

# Show help
show_help() {
  echo ""
  echo "Usage: $0 -ip <IP address> -f <path to .bin file> [-pw <OTA password>]"
  echo ""
  echo "Options:"
  echo "  -ip     IP address of the ESP8266 (required)"
  echo "  -f      Path to the .bin firmware file (required)"
  echo "  -pw     OTA password (optional)"
  echo "  -help   Show this help message"
  echo ""
}

# Default values
IP=""
BINFILE=""
AUTH=""

# Parse arguments
while [[ $# -gt 0 ]]; do
  case "$1" in
    -ip)
      IP="$2"
      shift 2
      ;;
    -f)
      BINFILE="$2"
      shift 2
      ;;
    -pw)
      AUTH="$2"
      shift 2
      ;;
    -help|--help)
      show_help
      exit 0
      ;;
    *)
      echo "Unknown option: $1"
      show_help
      exit 1
      ;;
  esac
done

# Validate required arguments
if [[ -z "$IP" || -z "$BINFILE" ]]; then
  echo "Missing required parameters."
  show_help
  exit 1
fi

# Build OTA command
# Try local version first
if [[ -f "./espota.py" ]]; then
  ESPOTA_PATH="./espota.py"
else
  # Fallback: find latest espota.py in Arduino core folder
  ESPOTA_PATH=$(find "$HOME/Library/Arduino15/packages/esp8266/hardware/esp8266" -name espota.py | sort -V | tail -n1)
fi

# Validate result
if [[ ! -f "$ESPOTA_PATH" ]]; then
  echo "espota.py not found in current directory or Arduino core installation."
  exit 1
fi

echo "Using espota.py at: $ESPOTA_PATH"

# Wait for device to become reachable via ping
echo "Waiting for $IP to respond to ping..."
while ! ping -c 1 -W 1 "$IP" >/dev/null 2>&1; do
  sleep 1
done

echo "$IP is reachable. Waiting briefly before OTA..."
sleep 2  # still useful, avoids premature start

MAX_RETRIES=10
RETRY_DELAY=1

echo "Trying OTA upload to $IP (up to $MAX_RETRIES attempts)..."

for ((i=1; i<=MAX_RETRIES; i++)); do
  echo "Attempt $i..."
  python3 "$ESPOTA_PATH" -i "$IP" -p 8266 --auth="$AUTH" -f "$BINFILE" && {
    echo "OTA upload successful on attempt $i."
    exit 0
  }
  echo "OTA upload failed. Retrying in $RETRY_DELAY seconds..."
  sleep $RETRY_DELAY
done

echo "OTA upload failed after $MAX_RETRIES attempts."
exit 1

