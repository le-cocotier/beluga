/**
 * @file project_files.c
 * @brief Walking a project's files, for the file finder and project search.
 */

#include "../include/project_files.h"
#include "../include/galloc.h"
#include "../include/os.h"
#include "../include/subprocess.h"

#include <stdio.h>
#include <string.h>

int projectIsGitRepo(const char *root)
{
    char *argv[] = {"git", "-C", (char *)root, "rev-parse", "--is-inside-work-tree", NULL};
    char *out = runArgvCaptureQuiet(argv, 1000, 64, NULL);
    int yes;
    if (!out)
        return 0;
    yes = strncmp(out, "true", 4) == 0;
    gfree(out);
    return yes;
}

static int isPruned(const char *name)
{
    static const char *const pruned[] = {".git", "node_modules", "build", "dist", ".beluga", NULL};
    int i;
    for (i = 0; pruned[i]; i++)
        if (strcmp(name, pruned[i]) == 0)
            return 1;
    return 0;
}

static int walk(const char *root, const char *rel, ProjectFileVisitor visit, void *ctx)
{
    char dir_path[OS_PATH_MAX];
    char child_rel[OS_PATH_MAX];
    char child_path[OS_PATH_MAX];
    OsDirEntry *entries;
    OsFileInfo info;
    int count = 0;
    int stop = 0;
    int i;

    snprintf(dir_path, sizeof(dir_path), "%s%s%s", root, *rel ? "/" : "", rel);
    entries = os_list_dir(dir_path, &count);
    if (!entries)
        return 0;
    for (i = 0; i < count && !stop; i++)
    {
        snprintf(child_rel, sizeof(child_rel), "%s%s%s", rel, *rel ? "/" : "", entries[i].name);
        snprintf(child_path, sizeof(child_path), "%s/%s", root, child_rel);
        if (entries[i].is_dir)
        {
            // Not into symbolic links to directories: they can loop.
            if (!isPruned(entries[i].name) && os_file_info(child_path, &info) == 0 && !info.is_link)
                stop = walk(root, child_rel, visit, ctx);
        }
        else
            stop = visit(child_rel, child_path, ctx);
    }
    os_free_dir(entries, count);
    return stop;
}

int projectWalkFiles(const char *root, const ProjectFileVisitor visit, void *ctx)
{
    return walk(root, "", visit, ctx);
}
