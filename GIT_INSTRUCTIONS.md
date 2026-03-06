# Git Commit Instructions

## Quick Commit (Recommended)

Use the provided script for an interactive commit process:

```bash
chmod +x commit_changes.sh
./commit_changes.sh
```

This script will:
1. Check git status
2. Stage all changes
3. Let you choose between full/short/custom commit message
4. Make the commit
5. Show next steps

---

## Manual Commit (Alternative)

If you prefer to do it manually:

### Option 1: Full Detailed Commit

```bash
# Stage all changes
git add -A

# Commit with the full template
git commit -F COMMIT_TEMPLATE.txt

# Optionally edit before committing
git commit -e -F COMMIT_TEMPLATE.txt
```

### Option 2: Short Commit Message

```bash
# Stage all changes
git add -A

# Commit with short message
git commit -m "feat: Add NUMA-based memory disaggregation support alongside RDMA" \
           -m "" \
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
```

### Option 3: Interactive Commit

```bash
# Stage all changes
git add -A

# Open editor for custom message
git commit
```

---

## Verify Commit

After committing, verify it looks good:

```bash
# View the commit
git show HEAD

# Check what will be pushed
git log --oneline -1

# See file changes
git diff HEAD~1 HEAD --stat
```

---

## Push Changes

### To existing branch:

```bash
# Push to current branch
git push origin $(git branch --show-current)
```

### To new branch (recommended):

```bash
# Create and switch to new branch
git checkout -b numa-support

# Push to new branch
git push -u origin numa-support
```

Then create a Pull Request on GitHub/GitLab.

---

## Files Changed Summary

This commit includes:

**Added (13 files):**
- xutils/numa_memory.hh
- xcomm/src/transport/numa_transport.hh
- outback/trait_numa.hpp
- outback/outback_server_numa.hh
- outback/outback_client_numa.hh
- benchs/outback/server_numa.cc
- benchs/outback/client_numa.cc
- build_numa.sh
- quickstart_numa.sh
- README_NUMA.md
- SETUP_GUIDE.md
- MIGRATION_SUMMARY.md
- COMMIT_TEMPLATE.txt
- commit_changes.sh
- GIT_INSTRUCTIONS.md (this file)

**Modified (3 files):**
- README.md (added NUMA sections)
- setup-env.sh (added numa mode)
- CMakeLists.txt (added NUMA targets)

**Unchanged:**
- All original RDMA code
- All existing benchmarks
- All dependencies

---

## Commit Message Template

The full commit template is available in `COMMIT_TEMPLATE.txt` and includes:

- **Subject**: feat: Add NUMA-based memory disaggregation support alongside RDMA
- **Body**: Detailed explanation of changes
- **Motivation**: Why NUMA support was added
- **Changes**: List of new/modified files
- **Usage**: How to use both NUMA and RDMA versions
- **Comparison**: NUMA vs RDMA table
- **Testing**: What was tested
- **Breaking Changes**: None (additive only)
- **Dependencies**: What's required
- **Future Work**: Potential enhancements

---

## Commit Best Practices

This commit follows conventional commits format:

```
<type>(<scope>): <subject>

<body>

<footer>
```

Where:
- **type**: feat (new feature)
- **scope**: optional (could be: numa, memory, infrastructure)
- **subject**: Short description (max 72 chars)
- **body**: Detailed explanation with context
- **footer**: Breaking changes, references, sign-off

---

## After Committing

1. **Test the commit:**
   ```bash
   # Clone fresh and test
   git clone <your-repo> test-clone
   cd test-clone
   ./quickstart_numa.sh
   ```

2. **Update branch:**
   ```bash
   # Amend if needed (before pushing)
   git commit --amend
   
   # Force push if already pushed
   git push --force-with-lease
   ```

3. **Tag the release (optional):**
   ```bash
   git tag -a v1.0.0-numa -m "Release with NUMA support"
   git push origin v1.0.0-numa
   ```

---

## Troubleshooting

**Problem**: "Changes not staged for commit"
```bash
git add -A
git status  # Verify files are staged
```

**Problem**: "File too large"
```bash
# Check for large files
git ls-files -z | xargs -0 du -h | sort -rh | head -20

# Remove if needed
git rm --cached large_file
echo "large_file" >> .gitignore
```

**Problem**: "Merge conflict"
```bash
# Pull latest changes first
git pull --rebase origin main

# Resolve conflicts, then
git add .
git rebase --continue
```

---

## Quick Reference

```bash
# Stage everything
git add -A

# Commit with template
git commit -F COMMIT_TEMPLATE.txt

# View commit
git show HEAD

# Push to new branch
git checkout -b numa-support
git push -u origin numa-support

# Or use the script
chmod +x commit_changes.sh
./commit_changes.sh
```
