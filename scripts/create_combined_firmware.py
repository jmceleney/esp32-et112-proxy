#!/usr/bin/env python3
"""
Combined Firmware + Filesystem Binary Generator

This script creates a single binary file containing both firmware and filesystem
for atomic updates via the ESP32 web interface.

The combined binary includes:
1. Bootloader
2. Partition table  
3. Application firmware
4. LittleFS filesystem

This ensures both firmware and web interface are updated together atomically.
"""

import os
import sys
import subprocess
import datetime

# Import PlatformIO environment
Import("env")

def debug_log(message):
    """Log debug message to console"""
    print(f"COMBINED-BUILD: {message}")

def run_command(command):
    """Run a shell command and return its output"""
    debug_log(f"Running: {command}")
    try:
        result = subprocess.run(
            command, 
            shell=True, 
            check=True, 
            stdout=subprocess.PIPE, 
            stderr=subprocess.PIPE,
            text=True
        )
        return result.stdout.strip()
    except subprocess.CalledProcessError as e:
        debug_log(f"Command failed: {e.stderr}")
        return None

def create_combined_firmware(source, target, env):
    """Create combined firmware binary after successful build"""
    debug_log("Starting combined firmware creation")
    
    # Get build environment and paths
    build_env = env.get("PIOENV")
    project_dir = env.get("PROJECT_DIR")
    build_dir = os.path.join(project_dir, ".pio", "build", build_env)
    
    # Check if required files exist
    firmware_bin = os.path.join(build_dir, "firmware.bin")
    littlefs_bin = os.path.join(build_dir, "littlefs.bin")
    bootloader_bin = os.path.join(build_dir, "bootloader.bin")
    partitions_bin = os.path.join(build_dir, "partitions.bin")
    
    # Find bootloader in ESP32 SDK if not in build dir
    if not os.path.exists(bootloader_bin):
        # Try to find bootloader from ESP32 framework
        framework_dir = env.get("PIOFRAMEWORK_DIR", "")
        if framework_dir:
            bootloader_bin = os.path.join(framework_dir, "tools", "sdk", "esp32", "bin", "bootloader_dio_40m.bin")
        
        if not os.path.exists(bootloader_bin):
            debug_log("Warning: Bootloader binary not found, combined firmware may not boot correctly")
            bootloader_bin = None
    
    # Check if LittleFS image exists - if not, build it
    if not os.path.exists(littlefs_bin):
        debug_log("LittleFS image not found, building filesystem...")
        result = run_command(f"pio run -e {build_env} -t buildfs")
        if result is None:
            debug_log("Failed to build LittleFS filesystem")
            return
    
    if not os.path.exists(firmware_bin):
        debug_log("ERROR: Firmware binary not found!")
        return
        
    if not os.path.exists(littlefs_bin):
        debug_log("ERROR: LittleFS binary not found!")
        return
    
    # Create combined binary using esptool.py
    combined_bin = os.path.join(build_dir, "firmware_combined.bin")
    
    debug_log(f"Creating combined binary: {combined_bin}")
    
    # Find esptool from PlatformIO packages
    platformio_packages = env.get("PIOPACKAGES_DIR", "")
    if platformio_packages:
        esptool_path = os.path.join(platformio_packages, "tool-esptoolpy", "esptool.py")
    else:
        # Fallback to known PlatformIO location
        home_dir = os.path.expanduser("~")
        esptool_path = os.path.join(home_dir, ".platformio", "packages", "tool-esptoolpy", "esptool.py")
    
    # Verify esptool exists
    if not os.path.exists(esptool_path):
        # Final fallback to system esptool
        esptool_path = "esptool.py"
    
    # Build esptool command for merging binaries  
    esptool_cmd = ["python", esptool_path, "--chip", "esp32", "merge_bin", "-o", combined_bin]
    
    # Add bootloader if available
    if bootloader_bin and os.path.exists(bootloader_bin):
        esptool_cmd.extend(["--flash_mode", "dio", "--flash_freq", "40m", "--flash_size", "4MB"])
        esptool_cmd.extend(["0x1000", bootloader_bin])
    
    # Add partition table if available  
    if os.path.exists(partitions_bin):
        esptool_cmd.extend(["0x8000", partitions_bin])
    
    # Add main firmware
    esptool_cmd.extend(["0x10000", firmware_bin])
    
    # Add filesystem
    # Note: Filesystem address depends on partition scheme, typically 0x290000 for default
    esptool_cmd.extend(["0x290000", littlefs_bin])
    
    # Execute esptool command
    try:
        result = subprocess.run(esptool_cmd, check=True, capture_output=True, text=True)
        debug_log("Combined binary created successfully!")
        debug_log(f"Output: {combined_bin}")
        
        # Show file size
        size = os.path.getsize(combined_bin)
        debug_log(f"Combined binary size: {size:,} bytes ({size/1024/1024:.1f} MB)")
        
        # Copy to project root for easy access
        root_combined = os.path.join(project_dir, "firmware_combined.bin")
        import shutil
        shutil.copy2(combined_bin, root_combined)
        debug_log(f"Combined binary copied to: firmware_combined.bin")
            
    except subprocess.CalledProcessError as e:
        debug_log(f"Failed to create combined binary: {e.stderr}")
        debug_log("Make sure esptool.py is installed: pip install esptool")
    except FileNotFoundError:
        debug_log("esptool.py not found! Install with: pip install esptool")

# Add this as a post-action to run after firmware build
env.AddPostAction("$BUILD_DIR/firmware.bin", create_combined_firmware)

debug_log("Combined firmware generator loaded")