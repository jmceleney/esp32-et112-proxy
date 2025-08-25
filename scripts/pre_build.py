#!/usr/bin/env python3
"""
Pre-build script to extract Git information

This script runs before the build and extracts git information 
(commit hash, branch, etc.) to include in the firmware build.

Usage: Called automatically by PlatformIO before build
"""

import os
import sys
import subprocess
import datetime

# Add debug logging
def debug_log(message):
    """Log debug message to a file for troubleshooting"""
    print(f"PRE-BUILD: {message}")
    with open("pre_build_debug.log", "a") as f:
        timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        f.write(f"[{timestamp}] {message}\n")

debug_log("Pre-build script started")
debug_log(f"Working directory: {os.getcwd()}")
debug_log(f"Python executable: {sys.executable}")
debug_log(f"Platform: {sys.platform}")
# Handle case where __file__ might not be defined in SCons context
try:
    script_path = os.path.abspath(__file__)
except NameError:
    script_path = "unknown (SCons context)"
debug_log(f"Script path: {script_path}")

def run_command(command):
    """Run a shell command and return its output"""
    debug_log(f"Running command: {command}")
    try:
        result = subprocess.run(
            command, 
            shell=True, 
            check=True, 
            stdout=subprocess.PIPE, 
            stderr=subprocess.PIPE,
            text=True
        )
        output = result.stdout.strip()
        debug_log(f"Command output: {output}")
        return output
    except subprocess.CalledProcessError as e:
        debug_log(f"Command error: {e.stderr}")
        print(f"Error executing command: {command}")
        print(f"Error message: {e.stderr}")
        return "unknown"

def git_is_repo():
    """Check if current directory is a git repository"""
    result = run_command("git rev-parse --is-inside-work-tree")
    is_repo = result == "true"
    debug_log(f"Is git repo: {is_repo}")
    return is_repo

def git_get_hash():
    """Get current git commit hash"""
    hash_value = run_command("git rev-parse --short HEAD")
    debug_log(f"Git hash: {hash_value}")
    return hash_value

def git_get_branch():
    """Get current branch name"""
    branch = run_command("git rev-parse --abbrev-ref HEAD")
    debug_log(f"Git branch: {branch}")
    return branch

def git_get_tag():
    """Get latest tag on current commit if any"""
    tag = run_command("git describe --tags --abbrev=0 2>/dev/null || echo 'no-tag'")
    debug_log(f"Git tag: {tag}")
    return tag

def main():
    debug_log("Entering main function")
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    
    # Default version information if not in a git repo
    version_info = f"Build: {timestamp}"
    
    # Check if we're in a git repository
    if git_is_repo():
        git_hash = git_get_hash()
        git_branch = git_get_branch()
        git_tag = git_get_tag()
        
        # Create version string with git information
        version_info = f"Git: {git_branch}@{git_hash} ({git_tag}) - Build: {timestamp}"
    
    # Set environment variable for build process
    debug_log(f"Setting GIT_VERSION to: {version_info}")
    os.environ["GIT_VERSION"] = version_info
    
    # Also create a header file as backup
    header_content = f'#ifndef VERSION_H\n#define VERSION_H\n#define FIRMWARE_VERSION "{version_info}"\n#endif\n'
    try:
        with open("include/version.h", "w") as f:
            f.write(header_content)
        debug_log("Created version.h header file")
    except Exception as e:
        debug_log(f"Failed to create version.h: {e}")
    
    # This can be used in the C++ code with the GIT_VERSION define
    debug_log("Pre-build script completed successfully")

if __name__ == "__main__":
    debug_log("Script execution started")
    try:
        main()
        debug_log("Script completed successfully")
    except Exception as e:
        debug_log(f"Unhandled exception: {str(e)}")
        raise 