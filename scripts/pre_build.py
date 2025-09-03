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

timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")

# Default version information if not in a git repo
version_info = f"Build: {timestamp}"
exact_tag = ""  # Initialize for non-git scenarios

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
{release_define}#endif
'''
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

# Build frontend if it exists and has changed
def build_frontend():
    """Build the Preact frontend if necessary"""
    web_dir = "web"
    data_dir = "data"
    
    # Check if web directory exists
    if not os.path.exists(web_dir):
        debug_log("No web directory found, skipping frontend build")
        return
    
    # Check if package.json exists
    package_json = os.path.join(web_dir, "package.json")
    if not os.path.exists(package_json):
        debug_log("No package.json found in web directory, skipping frontend build")
        return
    
    debug_log("Frontend directory detected, checking if build is needed")
    
    # Create data directory if it doesn't exist
    if not os.path.exists(data_dir):
        os.makedirs(data_dir)
        debug_log("Created data directory")
    
    # Check if node_modules exists, if not install dependencies
    node_modules_dir = os.path.join(web_dir, "node_modules")
    if not os.path.exists(node_modules_dir):
        debug_log("Installing frontend dependencies...")
        try:
            subprocess.run(["npm", "install"], cwd=web_dir, check=True)
            debug_log("Frontend dependencies installed successfully")
        except subprocess.CalledProcessError as e:
            debug_log(f"Failed to install frontend dependencies: {e}")
            return
        except FileNotFoundError:
            debug_log("npm not found, skipping frontend build")
            return
    
    # Build the frontend
    debug_log("Building frontend...")
    try:
        subprocess.run(["npm", "run", "build"], cwd=web_dir, check=True)
        debug_log("Frontend built successfully")
    except subprocess.CalledProcessError as e:
        debug_log(f"Frontend build failed: {e}")
    except FileNotFoundError:
        debug_log("npm not found, skipping frontend build")

# Build frontend
build_frontend()

debug_log("Pre-build script completed successfully")