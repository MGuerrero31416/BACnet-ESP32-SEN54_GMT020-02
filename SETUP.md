# ESP32 BACnet Project - Setup Guide

This guide explains how to set up this project on a new computer.

## Prerequisites

### 1. Install ESP-IDF
The project requires ESP-IDF v5.5.1. Install it using the official Espressif installer or the setup instructions at:

- https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/windows-setup.html

### 3. Install Python 3.11+
The ESP-IDF environment uses Python
- Download: https://www.python.org/downloads/
- Ensure "Add Python to PATH" is checked during installation

## Building the Project

### Using VS Code (Easiest)

1. **Open the project folder** in VS Code
2. **Install ESP-IDF Extension** if not already installed
   - Search for "Espressif IDF" in Extensions
3. **Open Command Palette** (`Ctrl+Shift+P`)
4. **Run**: `ESP-IDF: Configure ESP-IDF extension`
   - Let it auto-detect your ESP-IDF installation
5. **Run**: `ESP-IDF: Build` to compile

### Using Command Line

```bash
# Initialize ESP-IDF environment
C:\path\to\esp-idf\export.bat

# Build the project
cd C:\path\to\BACnet-ESP32-SEN54_HW657A
idf.py build

# Flash to device
idf.py -p COM8 flash

# Monitor output
idf.py -p COM8 monitor
```

## Project Structure

- **`main/`** - Main application code
- **`components/bacnet-stack/`** - Local BACnet stack library (DO NOT remove)
- **`build/`** - Compiled output (auto-generated, ignored by version control)
- **`sdkconfig`** - Project configuration (auto-generated, not committed)

## Important Notes

### Why `.vscode/settings.json` is not committed

The `.vscode/settings.json` file contains computer-specific paths and is excluded from version control. The VS Code ESP-IDF extension automatically detects your ESP-IDF installation on first use. If you need custom settings:

1. Copy the checked-in `.vscode/settings.json.example` (if it exists)
2. Or manually configure in VS Code:
   - Open Settings (`Ctrl+,`)
   - Search for "idf.espIdfPath"
   - Let the extension auto-detect

### Component Management

The local BACnet stack component in `components/bacnet-stack/` is crucial. The `idf_component.yml` is configured to use only this local version to avoid dependency conflicts.

## Troubleshooting

### Build fails with "cmake not found"
- Ensure ESP-IDF export script was run: `esp-idf\export.bat`

### Python dependency errors
- Run: `esp-idf\install.bat all`

### Component linking issues
- Ensure you have the **local** `components/bacnet-stack/` folder
- Do NOT delete this folder or rely on an online copy alone

### Can't connect to ESP32
- Check port in `.vscode/settings.json` (or auto-detect)
- Verify USB-to-Serial driver is installed
- Use: `idf.py -p COM8 flash` (replace COM8 with your port)

## Porting to Another Computer

When moving this project to another computer:

1. **Copy the entire folder** (including hidden `.vscode` folder)
2. **Run**: `idf.py fullclean` to remove build artifacts
3. **Delete** `sdkconfig` and `build/` directory if present
4. **Open in VS Code** and let the ESP-IDF extension configure paths automatically
5. **Build**: `idf.py build`

No manual path configuration should be needed!
