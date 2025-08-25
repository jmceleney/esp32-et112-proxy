#!/usr/bin/env python3
"""
Automatic post-build Git commit script

This script runs after a successful build and:
1. Checks if there are any uncommitted changes
2. Creates a commit with build information
3. Creates a tag with the build timestamp

Usage: Called automatically by PlatformIO after successful build
"""

import os
import sys
import subprocess
import datetime
import re
import hashlib

# Add debug logging 
def debug_log(message):
    """Log debug message to a file for troubleshooting"""
    print(f"POST-BUILD: {message}")
    with open("post_build_debug.log", "a") as f:
        timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
        f.write(f"[{timestamp}] {message}\n")

debug_log("Post-build script started")
debug_log(f"Working directory: {os.getcwd()}")
debug_log(f"Python executable: {sys.executable}")
debug_log(f"Environment: {os.environ.get('PIOENV', 'unknown')}")

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
        return None

def git_is_repo():
    """Check if current directory is a git repository"""
    result = run_command("git rev-parse --is-inside-work-tree")
    is_repo = result == "true"
    debug_log(f"Is git repo: {is_repo}")
    return is_repo

def git_has_changes():
    """Check if there are uncommitted changes"""
    result = run_command("git status --porcelain")
    has_changes = result != ""
    debug_log(f"Has git changes: {has_changes}")
    if has_changes:
        debug_log(f"Changes: {result}")
    return has_changes

def git_get_branch():
    """Get current branch name"""
    branch = run_command("git rev-parse --abbrev-ref HEAD")
    debug_log(f"Current branch: {branch}")
    return branch

def git_commit_build(build_env, commit_message, tag_name=None):
    """Commit the current state with build information"""
    # Stage all changes
    debug_log("Staging changes")
    run_command("git add -A")
    
    # Create commit with message
    debug_log(f"Creating commit with message: {commit_message}")
    result = run_command(f'git commit -m "{commit_message}"')
    if not result:
        debug_log("Failed to create commit")
        print("Failed to create commit")
        return False
    
    # Create tag if specified
    if tag_name:
        debug_log(f"Creating tag: {tag_name}")
        run_command(f'git tag -a {tag_name} -m "Build {tag_name}"')
        print(f"Created tag: {tag_name}")
    
    return True

def generate_build_hash():
    """Generate a short hash based on timestamp for unique build identification"""
    timestamp = datetime.datetime.now().isoformat()
    hash_obj = hashlib.md5(timestamp.encode())
    build_hash = hash_obj.hexdigest()[:8]
    debug_log(f"Generated build hash: {build_hash}")
    return build_hash  # Return first 8 chars of hash

def main():
    debug_log("Entering main function")
    
    # Check if we're in a git repository
    if not git_is_repo():
        debug_log("Not in a git repository. Skipping post-build commit.")
        print("Not in a git repository. Skipping post-build commit.")
        return 0
    
    # Get build environment name
    build_env = os.environ.get("PIOENV", "unknown")
    debug_log(f"Build environment: {build_env}")
    
    # Check if we have uncommitted changes
    if not git_has_changes():
        debug_log("No changes to commit. Build already tracked in git.")
        print("No changes to commit. Build already tracked in git.")
        return 0
    
    # Get current branch
    branch = git_get_branch()
    if branch == "HEAD":
        debug_log("Not on a branch (detached HEAD). Skipping post-build commit.")
        print("Not on a branch (detached HEAD). Skipping post-build commit.")
        return 0
    
    # Generate build information
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    build_hash = generate_build_hash()
    build_id = f"build_{timestamp}_{build_hash}"
    debug_log(f"Build ID: {build_id}")
    
    # Create commit message
    commit_message = f"Auto-commit after successful build [{build_env}]\\n\\nBuild ID: {build_id}\\nTimestamp: {timestamp}\\nEnvironment: {build_env}"
    debug_log(f"Commit message: {commit_message}")
    
    # Create commit and tag
    if git_commit_build(build_env, commit_message, build_id):
        debug_log(f"Successfully created build commit and tag: {build_id}")
        print(f"Successfully created build commit and tag: {build_id}")
    else:
        debug_log("Failed to create build commit")
        print("Failed to create build commit")
        return 1
    
    return 0

if __name__ == "__main__":
    debug_log("Script execution started")
    try:
        exit_code = main()
        debug_log(f"Script completed with exit code: {exit_code}")
        sys.exit(exit_code)
    except Exception as e:
        debug_log(f"Unhandled exception: {str(e)}")
        raise 