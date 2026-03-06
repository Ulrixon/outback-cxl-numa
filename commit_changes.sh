#!/bin/bash

# Git commit helper script for NUMA migration changes
# This script stages all changes and commits with the template

set -e

echo "=============================================="
echo "Preparing to commit NUMA migration changes"
echo "=============================================="
echo ""

# Check if we're in a git repo
if ! git rev-parse --git-dir > /dev/null 2>&1; then
    echo "Error: Not in a git repository"
    exit 1
fi

# Show status
echo "Current git status:"
echo "-------------------"
git status --short
echo ""

# Ask for confirmation
read -p "Do you want to stage and commit all these changes? (y/N) " -n 1 -r
echo
if [[ ! $REPLY =~ ^[Yy]$ ]]; then
    echo "Aborted."
    exit 1
fi

# Stage all changes
echo ""
echo "Staging changes..."
git add -A

# Show what will be committed
echo ""
echo "Files to be committed:"
echo "----------------------"
git diff --cached --name-status
echo ""

# Count changes
ADDED=$(git diff --cached --name-status | grep '^A' | wc -l)
MODIFIED=$(git diff --cached --name-status | grep '^M' | wc -l)
DELETED=$(git diff --cached --name-status | grep '^D' | wc -l)

echo "Summary: $ADDED added, $MODIFIED modified, $DELETED deleted"
echo ""

# Ask which commit style to use
echo "Choose commit style:"
echo "1) Full detailed commit (recommended)"
echo "2) Short commit message"
echo "3) Custom message"
read -p "Choice (1-3): " -n 1 -r CHOICE
echo ""

case $CHOICE in
    1)
        # Use the full template
        echo "Using detailed commit template..."
        git commit -F COMMIT_TEMPLATE.txt
        ;;
    2)
        # Short message
        echo "Using short commit message..."
        git commit -m "feat: Add NUMA-based memory disaggregation support alongside RDMA" \
                   -m "Add NUMA support as an alternative to RDMA for memory disaggregation." \
                   -m "Original RDMA functionality remains fully intact." \
                   -m "" \
                   -m "Key changes:" \
                   -m "- New NUMA memory management and transport layers" \
                   -m "- NUMA-based client/server implementations" \
                   -m "- Enhanced setup scripts with 'numa' mode" \
                   -m "- Comprehensive documentation" \
                   -m "" \
                   -m "Usage: ./setup-env.sh numa && ./build_numa.sh"
        ;;
    3)
        # Let user write custom message
        echo "Opening editor for custom commit message..."
        git commit
        ;;
    *)
        echo "Invalid choice. Aborting."
        exit 1
        ;;
esac

echo ""
echo "=============================================="
echo "Commit successful!"
echo "=============================================="
echo ""
echo "To view the commit:"
echo "  git show HEAD"
echo ""
echo "To push to remote:"
echo "  git push origin $(git branch --show-current)"
echo ""
echo "To create a new branch first:"
echo "  git checkout -b numa-support"
echo "  git push -u origin numa-support"
echo ""
