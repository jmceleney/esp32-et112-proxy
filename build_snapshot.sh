#!/bin/bash
# build_snapshot.sh - Build the project and create a git snapshot

# Check if PlatformIO is installed
if ! command -v pio &> /dev/null; then
    echo "PlatformIO is not installed. Please install it first."
    exit 1
fi

# Parse arguments
ENVIRONMENT="esp32debug"  # Default environment
while [[ "$#" -gt 0 ]]; do
    case $1 in
        -e|--env) ENVIRONMENT="$2"; shift ;;
        -h|--help)
            echo "Usage: $0 [-e|--env ENVIRONMENT]"
            echo "  -e, --env ENVIRONMENT    Specify build environment (default: esp32debug)"
            echo "  -h, --help               Show this help message"
            exit 0
            ;;
        *) echo "Unknown parameter: $1"; exit 1 ;;
    esac
    shift
done

echo "===================================================="
echo "Building project with environment: $ENVIRONMENT"
echo "===================================================="

# Make sure we're in a clean state first
echo "Checking git status..."
git status

# Build the project
echo "Building project..."
pio run -e "$ENVIRONMENT"

# Check if build was successful
if [ $? -ne 0 ]; then
    echo "Build failed. Not creating git snapshot."
    exit 1
fi

echo "Build completed successfully!"
echo "Git commit and tag were created automatically by the post-build script."
echo "Check 'git log' and 'git tag' to see the results."

# Display latest tag
echo "Latest tag:"
git describe --tags --abbrev=0

exit 0 