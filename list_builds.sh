#!/bin/bash
# list_builds.sh - List all build tags with detailed information

# Parse command-line options
SHOW_ALL=false
DETAIL_LEVEL=1

function show_help {
    echo "Usage: $0 [options]"
    echo
    echo "Options:"
    echo "  -a, --all     Show all builds (default: show only the 10 most recent)"
    echo "  -d, --detail  Show detailed information (1-3, default: 1)"
    echo "  -h, --help    Show this help message"
    echo
    echo "Detail levels:"
    echo "  1: Basic list with dates"
    echo "  2: More details including build environment"
    echo "  3: Full details with commit messages"
    exit 0
}

while [[ "$#" -gt 0 ]]; do
    case $1 in
        -a|--all) SHOW_ALL=true ;;
        -d|--detail) 
            if [[ "$2" =~ ^[1-3]$ ]]; then
                DETAIL_LEVEL=$2
                shift
            else
                DETAIL_LEVEL=2
            fi
            ;;
        -h|--help) show_help ;;
        *) echo "Unknown parameter: $1"; exit 1 ;;
    esac
    shift
done

# Check if git is installed
if ! command -v git &> /dev/null; then
    echo "Git is not installed. Please install it first."
    exit 1
fi

# Check if we're in a git repository
if ! git rev-parse --is-inside-work-tree &> /dev/null; then
    echo "Not in a git repository."
    exit 1
fi

# Count the number of build tags
BUILD_COUNT=$(git tag -l "build_*" | wc -l)

# Set up the header
echo "========================================================"
echo "ESP32-ET112-PROXY BUILD HISTORY"
echo "========================================================"
echo "Total builds: $BUILD_COUNT"
echo "========================================================"
echo

# Get the list of build tags
if [ "$SHOW_ALL" = true ]; then
    LIMIT=""
else
    LIMIT="| head -n 10"
fi

# Display builds based on detail level
case $DETAIL_LEVEL in
    1)
        # Basic list with dates
        echo "Recent builds (newest first):"
        echo "----------------------------------------"
        eval "git for-each-ref --sort=-creatordate --format='%(creatordate:short) %(refname:short)' refs/tags/build_* $LIMIT"
        ;;
    2)
        # More details including environment
        echo "Recent builds with details (newest first):"
        echo "----------------------------------------"
        TAGS=$(eval "git tag -l 'build_*' --sort=-creatordate $LIMIT")
        
        for tag in $TAGS; do
            echo "Build: $tag"
            echo "Date: $(git log -1 --format=%cd --date=local $tag)"
            echo "Commit: $(git rev-parse --short $tag^{commit})"
            
            # Extract environment from tag message if available
            ENV=$(git tag -l --format='%(contents)' $tag | grep -oP 'environment \K[^\s]+' || echo "unknown")
            echo "Environment: $ENV"
            echo "----------------------------------------"
        done
        ;;
    3)
        # Full details with commit messages
        echo "Recent builds with full details (newest first):"
        echo "========================================================"
        TAGS=$(eval "git tag -l 'build_*' --sort=-creatordate $LIMIT")
        
        for tag in $TAGS; do
            echo "Build: $tag"
            echo "Date: $(git log -1 --format=%cd --date=local $tag)"
            echo "Commit: $(git rev-parse --short $tag^{commit})"
            
            # Get tag message
            echo "Tag message:"
            git tag -l --format='%(contents)' $tag | sed 's/^/  /'
            
            # Get commit message
            echo "Commit message:"
            git log -1 --format=%B $tag | sed 's/^/  /'
            
            echo "========================================================"
        done
        ;;
esac

exit 0 