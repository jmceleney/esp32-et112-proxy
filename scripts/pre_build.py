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

def git_get_exact_tag():
    """Get exact tag on current commit (not just latest)"""
    tag = run_command("git describe --exact-match --tags HEAD 2>/dev/null || echo ''")
    debug_log(f"Exact git tag: {tag}")
    return tag

def git_is_release_tag(tag):
    """Check if tag is a release version (starts with v followed by number)"""
    import re
    is_release = bool(re.match(r'^v\d+', tag))
    debug_log(f"Is release tag '{tag}': {is_release}")
    return is_release

debug_log("Pre-build script started")
debug_log(f"Working directory: {os.getcwd()}")

def get_most_recent_source_timestamp():
    """Find the most recently modified source file and use its timestamp"""
    import glob
    import os
    
    # Define source file patterns to check
    # Exclude generated files (version.h, version.json) and build artifacts
    source_patterns = [
        "src/**/*.cpp",
        "src/**/*.c", 
        "src/**/*.h",
        "include/*.h",
        "web/src/**/*.jsx",
        "web/src/**/*.js",
        "web/src/**/*.css",
        "web/public/**/*.*",
        "platformio.ini",
        "package.json"
    ]
    
    # Files to explicitly exclude
    exclude_files = {
        "include/version.h",
        "data/web/version.json",
        "web/public/version.json"
    }
    
    most_recent_time = 0
    most_recent_file = None
    
    for pattern in source_patterns:
        for filepath in glob.glob(pattern, recursive=True):
            # Normalize path for comparison
            filepath = filepath.replace("\\", "/")
            
            # Skip excluded files
            if filepath in exclude_files:
                continue
                
            # Skip directories
            if os.path.isdir(filepath):
                continue
                
            try:
                mtime = os.path.getmtime(filepath)
                if mtime > most_recent_time:
                    most_recent_time = mtime
                    most_recent_file = filepath
            except OSError:
                # File might have been deleted or is inaccessible
                continue
    
    if most_recent_file:
        # Convert timestamp to datetime
        dt = datetime.datetime.fromtimestamp(most_recent_time)
        timestamp_str = dt.strftime("%Y-%m-%d %H:%M:%S")
        debug_log(f"Most recent source file: {most_recent_file}")
        debug_log(f"Source file timestamp: {timestamp_str}")
        return timestamp_str
    else:
        # Fallback to current time if no source files found
        timestamp_str = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        debug_log(f"No source files found, using current time: {timestamp_str}")
        return timestamp_str

# Get timestamp based on most recent source file
timestamp = get_most_recent_source_timestamp()
debug_log(f"Using timestamp: {timestamp}")

# Default version information if not in a git repo
version_info = f"Build: {timestamp}"
exact_tag = ""  # Initialize for non-git scenarios
git_hash = "unknown"
git_branch = "unknown"

# Check if we're in a git repository
if git_is_repo():
    git_hash = git_get_hash()
    git_branch = git_get_branch()
    git_tag = git_get_tag()
    exact_tag = git_get_exact_tag()
    
    # If we're on a release tag, use simplified version string
    if exact_tag and git_is_release_tag(exact_tag):
        version_info = f"{exact_tag} - Build: {timestamp}"
    else:
        # For development builds, include full git information
        version_info = f"Git: {git_branch}@{git_hash} ({git_tag}) - Build: {timestamp}"

debug_log(f"Generated version: {version_info}")

# Create header file with release version if applicable
release_define = ""
if exact_tag and git_is_release_tag(exact_tag):
    release_define = f'#define RELEASE_VERSION "{exact_tag}"\n'

header_content = f'''#ifndef VERSION_H
#define VERSION_H
#define FIRMWARE_VERSION "{version_info}"
#define BUILD_TIMESTAMP "{timestamp}"
{release_define}#endif
'''

# Always create version.h with the consistent timestamp
try:
    with open("include/version.h", "w") as f:
        f.write(header_content)
    debug_log("Created/updated version.h header file")
except Exception as e:
    debug_log(f"Failed to create version.h: {e}")

# Add build flag to PlatformIO environment
try:
    debug_log("Adding GIT_VERSION build flag to PlatformIO environment")
    env.Append(CPPDEFINES=[("GIT_VERSION", f'\\"{version_info}\\"')])
    debug_log("Successfully added GIT_VERSION build flag")
except Exception as e:
    debug_log(f"Failed to add build flag: {e}")

# Create version.json for web interface
def create_web_version_file():
    """Create version.json file for web interface to detect sync"""
    # PlatformIO builds filesystem from data/ directory, not web/
    data_web_dir = os.path.join("data", "web")
    
    # Only create if data directory exists
    if not os.path.exists("data"):
        debug_log("No data directory found, skipping web version file creation")
        return
    
    # Create data/web directory if it doesn't exist
    if not os.path.exists(data_web_dir):
        os.makedirs(data_web_dir)
        debug_log("Created data/web directory")
    
    version_json_path = os.path.join(data_web_dir, "version.json")
    
    # Use the EXACT SAME timestamp as version.h
    version_json_content = {
        "filesystem_version": version_info,
        "build_time": timestamp,  # Same timestamp as version.h
        "description": "ESP32 ET112 Proxy Web Interface",
        "git_hash": git_hash,
        "git_branch": git_branch
    }
    
    try:
        import json
        with open(version_json_path, "w") as f:
            json.dump(version_json_content, f, indent=2)
        debug_log(f"Created web version file: {version_json_path}")
        debug_log(f"Web version: {version_info}")
    except Exception as e:
        debug_log(f"Failed to create web version file: {e}")

# Create web version file
create_web_version_file()

debug_log("Pre-build script completed successfully")