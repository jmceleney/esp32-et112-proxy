#!/bin/bash
# create_release.sh - Helper script to create a new release

# Color codes for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to print colored output
print_color() {
    color=$1
    message=$2
    echo -e "${color}${message}${NC}"
}

# Check if we're in a git repository
if ! git rev-parse --is-inside-work-tree > /dev/null 2>&1; then
    print_color $RED "Error: Not in a git repository"
    exit 1
fi

# Check for uncommitted changes
if ! git diff-index --quiet HEAD --; then
    print_color $YELLOW "Warning: You have uncommitted changes"
    echo "Current status:"
    git status --short
    echo ""
    read -p "Do you want to commit these changes first? (y/n) " -n 1 -r
    echo ""
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        read -p "Enter commit message: " commit_msg
        git add -A
        git commit -m "$commit_msg"
    else
        print_color $RED "Please commit or stash your changes before creating a release"
        exit 1
    fi
fi

# Get current version tag
current_tag=$(git describe --tags --abbrev=0 2>/dev/null || echo "v0.0.0")
print_color $GREEN "Current version: $current_tag"

# Parse current version (assuming vMAJOR.MINOR.PATCH format)
if [[ $current_tag =~ ^v?([0-9]+)\.([0-9]+)(\.([0-9]+))?$ ]]; then
    major="${BASH_REMATCH[1]}"
    minor="${BASH_REMATCH[2]}"
    patch="${BASH_REMATCH[4]:-0}"
else
    print_color $YELLOW "Could not parse current version, starting from v1.0.0"
    major=1
    minor=0
    patch=0
fi

# Suggest next versions
next_patch=$((patch + 1))
next_minor=$((minor + 1))
next_major=$((major + 1))

echo ""
echo "Select version bump type:"
echo "  1) Patch (v${major}.${minor}.${next_patch}) - Bug fixes"
echo "  2) Minor (v${major}.${next_minor}.0) - New features"  
echo "  3) Major (v${next_major}.0.0) - Breaking changes"
echo "  4) Custom version"
echo ""
read -p "Enter choice (1-4): " choice

case $choice in
    1)
        new_version="v${major}.${minor}.${next_patch}"
        ;;
    2)
        new_version="v${major}.${next_minor}.0"
        ;;
    3)
        new_version="v${next_major}.0.0"
        ;;
    4)
        read -p "Enter custom version (e.g., v1.2.3): " new_version
        # Validate format
        if ! [[ $new_version =~ ^v[0-9]+\.[0-9]+(\.[0-9]+)?$ ]]; then
            print_color $RED "Invalid version format. Use vX.Y or vX.Y.Z"
            exit 1
        fi
        ;;
    *)
        print_color $RED "Invalid choice"
        exit 1
        ;;
esac

# Check if tag already exists
if git rev-parse "$new_version" >/dev/null 2>&1; then
    print_color $RED "Error: Tag $new_version already exists"
    exit 1
fi

print_color $GREEN "Creating release: $new_version"
echo ""

# Get release notes
echo "Enter release notes (press Ctrl+D when done):"
release_notes=$(cat)

if [ -z "$release_notes" ]; then
    print_color $YELLOW "No release notes provided, using default message"
    release_notes="Release version ${new_version#v}"
fi

# Build the project first
echo ""
print_color $YELLOW "Building project to verify it compiles..."
if ! ./build_snapshot.sh -e esp32release; then
    print_color $RED "Build failed! Fix errors before creating release."
    exit 1
fi

# Create annotated tag
echo ""
print_color $GREEN "Creating git tag..."
git tag -a "$new_version" -m "$release_notes"

# Show the created tag
echo ""
print_color $GREEN "Tag created successfully!"
git show "$new_version" --no-patch

# Ask about pushing to remote
echo ""
read -p "Push tag to remote? (y/n) " -n 1 -r
echo ""
if [[ $REPLY =~ ^[Yy]$ ]]; then
    git push origin "$new_version"
    print_color $GREEN "Tag pushed to remote"
    
    echo ""
    echo "To create a GitHub release:"
    echo "  1. Go to https://github.com/jmceleney/esp32-et112-proxy/releases/new"
    echo "  2. Select tag: $new_version"
    echo "  3. Add release title and notes"
    echo "  4. Upload firmware binaries from .pio/build/esp32release/"
    echo ""
    echo "Or use GitHub CLI:"
    echo "  gh release create $new_version --title \"$new_version\" --notes \"$release_notes\""
else
    print_color $YELLOW "Tag created locally. Push with: git push origin $new_version"
fi

print_color $GREEN "Done!"