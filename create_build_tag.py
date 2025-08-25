#!/usr/bin/env python3
"""
Create a build tag for the latest commit

This script manually creates a build tag for the most recent commit,
useful when the automatic post-build tagging failed to run.

Usage: python create_build_tag.py [environment_name]
"""

import os
import sys
import subprocess
import datetime
import hashlib

def run_command(command):
    """Run a shell command and return its output"""
    print(f"Running: {command}")
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
        print(f"Error executing command: {command}")
        print(f"Error message: {e.stderr}")
        return None

def git_is_repo():
    """Check if current directory is a git repository"""
    return run_command("git rev-parse --is-inside-work-tree") == "true"

def git_last_commit_hash():
    """Get the hash of the last commit"""
    return run_command("git rev-parse HEAD")

def generate_build_hash():
    """Generate a short hash based on timestamp for unique build identification"""
    timestamp = datetime.datetime.now().isoformat()
    hash_obj = hashlib.md5(timestamp.encode())
    return hash_obj.hexdigest()[:8]  # Return first 8 chars of hash

def create_build_tag(env_name):
    """Create a build tag for the latest commit"""
    if not git_is_repo():
        print("Not in a git repository.")
        return False
    
    # Get the hash of the last commit
    commit_hash = git_last_commit_hash()
    if not commit_hash:
        print("Failed to get the last commit hash.")
        return False
    
    # Generate build information
    timestamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
    build_hash = generate_build_hash()
    build_id = f"build_{timestamp}_{build_hash}"
    
    # Create the tag
    tag_message = f"Build {build_id} for environment {env_name}"
    result = run_command(f'git tag -a {build_id} -m "{tag_message}" {commit_hash}')
    
    if result is not None:
        print(f"Successfully created build tag: {build_id}")
        return True
    else:
        print("Failed to create build tag.")
        return False

def main():
    # Get environment name from command line argument or use default
    env_name = "unknown"
    if len(sys.argv) > 1:
        env_name = sys.argv[1]
    
    print(f"Creating build tag for environment: {env_name}")
    
    if create_build_tag(env_name):
        print("Build tag created successfully.")
        # Show list of recent tags
        print("\nRecent build tags:")
        run_command('git tag -l "build_*" --sort=-creatordate | head -n 5')
        return 0
    else:
        print("Failed to create build tag.")
        return 1

if __name__ == "__main__":
    sys.exit(main()) 