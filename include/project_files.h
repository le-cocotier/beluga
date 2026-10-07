#ifndef PROJECT_FILES_H_
#define PROJECT_FILES_H_

// ─── Walking a project ──────────────────────────────────────────────────────
// Shared by the file finder and project search: is a directory in a git
// work tree, and every file under one that isn't. Both work the same way
// on every platform — git when it's a repository, this walker otherwise.

// Is `root` inside a git work tree? Asks git rather than looking for a
// .git directory, which would get worktrees and submodules wrong.
int projectIsGitRepo(const char *root);

// Called with each file's path relative to the root ("src/main.c") and
// its full path. Return 0 to go on, anything else to stop the walk.
typedef int (*ProjectFileVisitor)(const char *rel_path, const char *full_path, void *ctx);

// Visits every regular file under `root`, skipping the directories nobody
// wants in a project listing (.git, node_modules, build, dist, .beluga).
// Returns what the visitor stopped with, or 0.
int projectWalkFiles(const char *root, ProjectFileVisitor visit, void *ctx);

#endif
