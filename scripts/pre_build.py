Import("env")

import os
import subprocess
import datetime

# Add debug logging first
def debug_log(message):
    """Log debug message to a file for troubleshooting"""
    print(f"PRE-BUILD: {message}")
    with open("pre_build_debug.log", "a") as f:
        timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        f.write(f"[{timestamp}] {message}\n")

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

debug_log("Pre-build script started")
debug_log(f"Working directory: {os.getcwd()}")

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

debug_log(f"Generated version: {version_info}")

# Create header file
header_content = f'#ifndef VERSION_H\n#define VERSION_H\n#define FIRMWARE_VERSION "{version_info}"\n#endif\n'
try:
    with open("include/version.h", "w") as f:
        f.write(header_content)
    debug_log("Created version.h header file")
except Exception as e:
    debug_log(f"Failed to create version.h: {e}")

# Add build flag to PlatformIO environment
try:
    debug_log("Adding GIT_VERSION build flag to PlatformIO environment")
    env.Append(CPPDEFINES=[("GIT_VERSION", f'\\"{version_info}\\"')])
    debug_log("Successfully added GIT_VERSION build flag")
except Exception as e:
    debug_log(f"Failed to add build flag: {e}")

debug_log("Pre-build script completed successfully")