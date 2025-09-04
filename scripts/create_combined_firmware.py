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

def get_build_timestamp(project_dir):
    """Extract build timestamp from version.h"""
    version_h_path = os.path.join(project_dir, "include", "version.h")
    
    if not os.path.exists(version_h_path):
        debug_log("version.h not found, cannot determine build timestamp")
        return None
    
    try:
        with open(version_h_path, 'r') as f:
            content = f.read()
            
        # Look for BUILD_TIMESTAMP define
        import re
        match = re.search(r'#define BUILD_TIMESTAMP "([^"]+)"', content)
        if match:
            timestamp_str = match.group(1)
            # Parse timestamp: "2025-09-04 08:32:58"
            from datetime import datetime
            timestamp = datetime.strptime(timestamp_str, "%Y-%m-%d %H:%M:%S")
            debug_log(f"Build timestamp from version.h: {timestamp_str}")
            return timestamp
        else:
            debug_log("BUILD_TIMESTAMP not found in version.h")
            return None
            
    except Exception as e:
        debug_log(f"Error reading build timestamp: {e}")
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
    
    # Get build timestamp from version.h
    build_timestamp = get_build_timestamp(project_dir)
    
    # Check if LittleFS needs to be built/rebuilt
    rebuild_fs = False
    
    if not os.path.exists(littlefs_bin):
        debug_log("LittleFS image not found - will build filesystem")
        rebuild_fs = True
    elif build_timestamp is not None:
        # Check if littlefs.bin is older than build timestamp
        from datetime import datetime
        littlefs_mtime = datetime.fromtimestamp(os.path.getmtime(littlefs_bin))
        
        if littlefs_mtime < build_timestamp:
            debug_log(f"LittleFS image is outdated (file: {littlefs_mtime}, build: {build_timestamp})")
            debug_log("Removing old filesystem image and rebuilding...")
            os.remove(littlefs_bin)
            rebuild_fs = True
        else:
            debug_log(f"LittleFS image is current (file: {littlefs_mtime}, build: {build_timestamp})")
    else:
        debug_log("Cannot determine build timestamp - using existing filesystem if available")
    
    if rebuild_fs:
        debug_log("Building filesystem to match firmware timestamp...")
        
        # Build web interface first
        debug_log("Building Preact web interface...")
        web_dir = os.path.join(project_dir, "web")
        if os.path.exists(web_dir):
            # Change to web directory and run npm build
            original_dir = os.getcwd()
            os.chdir(web_dir)
            try:
                npm_build_result = run_command("npm run build")
                if npm_build_result is None:
                    debug_log("ERROR: Failed to build web interface!")
                    debug_log("Cannot create combined firmware without web interface.")
                    return
                debug_log("Web interface built successfully")
            finally:
                # Always change back to original directory
                os.chdir(original_dir)
        else:
            debug_log("Warning: web directory not found, skipping npm build")
        
        # Build filesystem using PlatformIO
        buildfs_cmd = f"pio run -e {build_env} -t buildfs"
        result = run_command(buildfs_cmd)
        
        if result is None:
            debug_log("ERROR: Failed to build filesystem!")
            debug_log("Cannot create combined firmware without filesystem.")
            return
        
        # Verify filesystem was created
        if not os.path.exists(littlefs_bin):
            debug_log("ERROR: Filesystem build completed but littlefs.bin not found!")
            return
            
        debug_log("Filesystem built successfully - proceeding with combined firmware creation.")
    
    if not os.path.exists(firmware_bin):
        debug_log("ERROR: Firmware binary not found!")
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