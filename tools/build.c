// Self-bootstrapping build program. Bootstrap once with:
//     cc tools/build.c -o nob
// Afterwards ./nob rebuilds itself when this file changes.
//
//     ./nob                 debug binary with sanitizers  -> out/debug/bprocs
//     ./nob release         optimised binary              -> out/release/bprocs
//     ./nob test            build and run the tests (debug)
//     ./nob check           modern-c contract check, then the tests
//     ./nob cross           release binary for every target with zig cc
//     ./nob install         release binary              -> $DESTDIR$PREFIX/bin/bprocs
//     ./nob uninstall       remove $DESTDIR$PREFIX/bin/bprocs
//     ./nob clean           remove out/
//
// Host builds compile with $CC, or cc when it is unset. $PREFIX defaults to ~/.local.

#include <dirent.h>
#include <errno.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

enum { MAX_ARGS = 256, MAX_FILES = 128, PATH_MAX_LEN = 1024, FINGERPRINT_LEN = 4096 };

typedef struct {
    const char *items[MAX_ARGS];
    size_t count;
} Command;

typedef struct {
    char items[MAX_FILES][PATH_MAX_LEN];
    size_t count;
} FileList;

typedef struct {
    const char *name;
    const char *zig_target;
} Target;

static const Target TARGETS[] = {
    { "linux-x86_64", "x86_64-linux-gnu" },
    { "linux-aarch64", "aarch64-linux-gnu" },
    { "macos-aarch64", "aarch64-macos" },
};

static const char *const WARNING_FLAGS[] = {
    "-std=c23", "-Wall", "-Wextra", "-Werror", "-Wconversion", "-Wshadow", "-Wvla",
    "-Wstrict-prototypes", "-Wimplicit-fallthrough", "-Wno-unused-parameter",
};
static const char *const DEBUG_FLAGS[] = { "-g", "-O0", "-fsanitize=address,undefined", "-fno-omit-frame-pointer" };
static const char *const RELEASE_FLAGS[] = { "-O2" };
static const char *const LIBMC_AREAS[] = { "core", "text", "container", "crypto", "platform", "concurrency", "encoding", "log" };
// libmc's own vendored sources compile in their own standard without the
// warning flags: their warnings are upstream's to fix.
static const char *const LIBMC_DEPENDENCIES[] = {
    "deps/libmc/deps/cyaml", "deps/libmc/deps/yyjson", "deps/libmc/deps/utf8proc",
    "deps/libmc/deps/md4c",  "deps/libmc/deps/expat/lib",
};
// expat finds its config in libmc's src/encoding, and xmltok.c includes the
// sources in lib/included, which are not compiled alone.
static const char *const DEPENDENCY_FLAGS[] = {
    "-std=c11", "-w", "-Ideps/libmc/src/encoding", "-Ideps/libmc/deps/expat/lib/included",
};
static const char *const INCLUDE_FLAGS[] = {
    "-Isrc",
    "-Ideps/libmc/include",
    "-Ideps/libmc/deps/cyaml",
    "-Ideps/libmc/deps/yyjson",
    "-Ideps/libmc/deps/utf8proc",
    "-Ideps/libmc/deps/md4c",
    "-Ideps/libmc/deps/expat/lib",
};
static const char *const SOURCE_DIRECTORIES[] = { "src", "src/platform" };
static const char *const MAIN_SOURCE = "src/main.c";

static void command_push(Command *command, const char *item)
{
    if (command->count + 1 >= MAX_ARGS) {
        fprintf(stderr, "build: too many arguments\n");
        exit(2);
    }
    command->items[command->count++] = item;
    command->items[command->count] = NULL;
}

static void command_push_many(Command *command, const char *const *items, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        command_push(command, items[i]);
    }
}

static int command_run(const Command *command)
{
    for (size_t i = 0; i < command->count; i++) {
        printf("%s%s", i == 0 ? "" : " ", command->items[i]);
    }
    printf("\n");
    fflush(stdout);
    pid_t pid;
    int status = posix_spawnp(&pid, command->items[0], NULL, NULL, (char *const *)command->items, environ);
    if (status != 0) {
        fprintf(stderr, "build: cannot run %s: %s\n", command->items[0], strerror(status));
        return 1;
    }
    int wait_status = 0;
    if (waitpid(pid, &wait_status, 0) < 0) {
        return 1;
    }
    return WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 1;
}

static void run_or_die(const Command *command)
{
    int status = command_run(command);
    if (status != 0) {
        fprintf(stderr, "build: command failed with status %d\n", status);
        exit(status);
    }
}

static time_t mtime_of(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 ? st.st_mtime : 0;
}

static void mkdir_p(const char *path)
{
    char buffer[PATH_MAX_LEN];
    snprintf(buffer, sizeof buffer, "%s", path);
    for (char *p = buffer + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(buffer, 0755);
            *p = '/';
        }
    }
    mkdir(buffer, 0755);
}

static void list_files(FileList *list, const char *directory, const char *extension)
{
    DIR *dir = opendir(directory);
    if (dir == NULL) {
        return;
    }
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t name_len = strlen(entry->d_name);
        size_t ext_len = strlen(extension);
        if (name_len <= ext_len || strcmp(entry->d_name + name_len - ext_len, extension) != 0) {
            continue;
        }
        if (list->count >= MAX_FILES) {
            fprintf(stderr, "build: too many files in %s\n", directory);
            exit(2);
        }
        snprintf(list->items[list->count++], PATH_MAX_LEN, "%s/%s", directory, entry->d_name);
    }
    closedir(dir);
}

static time_t newest_of(const FileList *files)
{
    time_t newest = 0;
    for (size_t i = 0; i < files->count; i++) {
        time_t t = mtime_of(files->items[i]);
        newest = t > newest ? t : newest;
    }
    return newest;
}

static time_t newest_header_mtime(void)
{
    FileList headers = { 0 };
    for (size_t i = 0; i < sizeof LIBMC_AREAS / sizeof *LIBMC_AREAS; i++) {
        char directory[PATH_MAX_LEN];
        snprintf(directory, sizeof directory, "deps/libmc/include/mc/%s", LIBMC_AREAS[i]);
        list_files(&headers, directory, ".h");
        snprintf(directory, sizeof directory, "deps/libmc/src/%s", LIBMC_AREAS[i]);
        list_files(&headers, directory, ".h");
    }
    for (size_t i = 0; i < sizeof LIBMC_DEPENDENCIES / sizeof *LIBMC_DEPENDENCIES; i++) {
        list_files(&headers, LIBMC_DEPENDENCIES[i], ".h");
    }
    for (size_t i = 0; i < sizeof SOURCE_DIRECTORIES / sizeof *SOURCE_DIRECTORIES; i++) {
        list_files(&headers, SOURCE_DIRECTORIES[i], ".h");
    }
    list_files(&headers, "tests", ".h");
    return newest_of(&headers);
}

typedef struct {
    bool release;
    bool dependency;
    const Target *target;
    char out_dir[PATH_MAX_LEN];
    char obj_dir[PATH_MAX_LEN];
    time_t headers_mtime;
} Build;

// host_compiler is $CC when set, so one checkout can build with clang and gcc.
static const char *host_compiler(void)
{
    const char *compiler = getenv("CC");
    return compiler != NULL && compiler[0] != '\0' ? compiler : "cc";
}

static void push_compiler(Command *command, const Build *build)
{
    if (build->target != NULL) {
        command_push(command, "zig");
        command_push(command, "cc");
        command_push(command, "-target");
        command_push(command, build->target->zig_target);
    } else {
        command_push(command, host_compiler());
    }
}

// Sanitizers need the host's runtime, so cross builds only compile.
static bool sanitized(const Build *build)
{
    return !build->release && build->target == NULL;
}

static void object_path(char *out, const char *obj_dir, const char *source)
{
    char flat[PATH_MAX_LEN];
    snprintf(flat, sizeof flat, "%s", source);
    for (char *p = flat; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '_';
        }
    }
    flat[strlen(flat) - 2] = '\0';
    snprintf(out, PATH_MAX_LEN, "%s/%s.o", obj_dir, flat);
}

// push_compile_command pushes everything but the input and output files.
static void push_compile_command(Command *command, const Build *build)
{
    push_compiler(command, build);
    if (build->dependency) {
        command_push_many(command, DEPENDENCY_FLAGS, sizeof DEPENDENCY_FLAGS / sizeof *DEPENDENCY_FLAGS);
    } else {
        command_push_many(command, WARNING_FLAGS, sizeof WARNING_FLAGS / sizeof *WARNING_FLAGS);
    }
    if (build->release) {
        command_push_many(command, RELEASE_FLAGS, sizeof RELEASE_FLAGS / sizeof *RELEASE_FLAGS);
    } else if (sanitized(build)) {
        command_push_many(command, DEBUG_FLAGS, sizeof DEBUG_FLAGS / sizeof *DEBUG_FLAGS);
    } else {
        command_push(command, "-g");
    }
    command_push_many(command, INCLUDE_FLAGS, sizeof INCLUDE_FLAGS / sizeof *INCLUDE_FLAGS);
}

static void command_text(const Command *command, char *out, size_t size)
{
    size_t at = 0;
    out[0] = '\0';
    for (size_t i = 0; i < command->count && at < size; i++) {
        int n = snprintf(out + at, size - at, "%s%s", i == 0 ? "" : " ", command->items[i]);
        at += n > 0 ? (size_t)n : 0;
    }
}

static bool file_holds(const char *path, const char *text)
{
    FILE *file = fopen(path, "r");
    if (file == NULL) {
        return false;
    }
    size_t len = strlen(text);
    size_t at = 0;
    bool same = true;
    int c;
    while (same && (c = fgetc(file)) != EOF) {
        same = at < len && (char)c == text[at++];
    }
    fclose(file);
    return same && at == len;
}

static void write_text(const char *path, const char *text)
{
    FILE *file = fopen(path, "w");
    if (file == NULL || fputs(text, file) < 0 || fclose(file) != 0) {
        fprintf(stderr, "build: cannot write %s\n", path);
        exit(1);
    }
}

// fingerprint describes what shapes objects besides their sources and headers:
// the compiler's version and the compile command.
static void fingerprint(const Build *build, char *out, size_t size)
{
    Command command = { 0 };
    push_compiler(&command, build);
    command_push(&command, "--version");
    char version_command[FINGERPRINT_LEN];
    command_text(&command, version_command, sizeof version_command);
    char version[FINGERPRINT_LEN / 2] = "";
    FILE *pipe = popen(version_command, "r");
    if (pipe != NULL) {
        version[fread(version, 1, sizeof version - 1, pipe)] = '\0';
        pclose(pipe);
    }
    Command compile_command = { 0 };
    push_compile_command(&compile_command, build);
    char flags[FINGERPRINT_LEN / 2];
    command_text(&compile_command, flags, sizeof flags);
    snprintf(out, size, "%s%s\n", version, flags);
}

// discard_stale_objects removes the objects when the fingerprint changed since they were built.
static void discard_stale_objects(const Build *build)
{
    char current[FINGERPRINT_LEN];
    fingerprint(build, current, sizeof current);
    char path[PATH_MAX_LEN];
    snprintf(path, sizeof path, "%s/fingerprint", build->out_dir);
    if (file_holds(path, current)) {
        return;
    }
    Command command = { 0 };
    command_push(&command, "rm");
    command_push(&command, "-rf");
    command_push(&command, build->obj_dir);
    run_or_die(&command);
    mkdir_p(build->obj_dir);
    write_text(path, current);
}

static void compile(const Build *build, const char *source, const char *object)
{
    if (mtime_of(object) > mtime_of(source) && mtime_of(object) > build->headers_mtime) {
        return;
    }
    Command command = { 0 };
    push_compile_command(&command, build);
    command_push(&command, "-c");
    command_push(&command, source);
    command_push(&command, "-o");
    command_push(&command, object);
    run_or_die(&command);
}

// compile_directory compiles every source in directory but skip into objects.
static void compile_directory(const Build *build, const char *directory, const char *skip, FileList *objects)
{
    FileList sources = { 0 };
    list_files(&sources, directory, ".c");
    for (size_t i = 0; i < sources.count; i++) {
        if (skip != NULL && strcmp(sources.items[i], skip) == 0) {
            continue;
        }
        char *object = objects->items[objects->count++];
        object_path(object, build->obj_dir, sources.items[i]);
        compile(build, sources.items[i], object);
    }
}

static void archive(const Build *build, const FileList *objects, const char *library)
{
    // The member list catches a removed source, which leaves every object older than the archive.
    char members[MAX_FILES * PATH_MAX_LEN];
    size_t at = 0;
    for (size_t i = 0; i < objects->count; i++) {
        at += (size_t)snprintf(members + at, sizeof members - at, "%s\n", objects->items[i]);
    }
    char members_path[PATH_MAX_LEN];
    snprintf(members_path, sizeof members_path, "%s.members", library);
    if (mtime_of(library) > newest_of(objects) && file_holds(members_path, members)) {
        return;
    }
    remove(library);
    Command command = { 0 };
    if (build->target != NULL) {
        command_push(&command, "zig");
    }
    command_push(&command, "ar");
    command_push(&command, "rcs");
    command_push(&command, library);
    for (size_t i = 0; i < objects->count; i++) {
        command_push(&command, objects->items[i]);
    }
    run_or_die(&command);
    write_text(members_path, members);
}

// build_libraries archives libmc, and the project without its main, so the
// binary and the tests link only the libmc modules they call.
static void build_libraries(const Build *build, char *libmc, char *project)
{
    mkdir_p(build->obj_dir);
    discard_stale_objects(build);

    FileList libmc_objects = { 0 };
    for (size_t i = 0; i < sizeof LIBMC_AREAS / sizeof *LIBMC_AREAS; i++) {
        char directory[PATH_MAX_LEN];
        snprintf(directory, sizeof directory, "deps/libmc/src/%s", LIBMC_AREAS[i]);
        compile_directory(build, directory, NULL, &libmc_objects);
    }
    Build dependency = *build;
    dependency.dependency = true;
    for (size_t i = 0; i < sizeof LIBMC_DEPENDENCIES / sizeof *LIBMC_DEPENDENCIES; i++) {
        compile_directory(&dependency, LIBMC_DEPENDENCIES[i], NULL, &libmc_objects);
    }
    snprintf(libmc, PATH_MAX_LEN, "%s/libmc.a", build->out_dir);
    archive(build, &libmc_objects, libmc);

    FileList project_objects = { 0 };
    for (size_t i = 0; i < sizeof SOURCE_DIRECTORIES / sizeof *SOURCE_DIRECTORIES; i++) {
        compile_directory(build, SOURCE_DIRECTORIES[i], MAIN_SOURCE, &project_objects);
    }
    snprintf(project, PATH_MAX_LEN, "%s/libbprocs.a", build->out_dir);
    archive(build, &project_objects, project);
}

static void link_binary(const Build *build, const FileList *objects, const char *project, const char *libmc,
                        const char *binary)
{
    if (mtime_of(binary) > newest_of(objects) && mtime_of(binary) > mtime_of(project) &&
        mtime_of(binary) > mtime_of(libmc)) {
        return;
    }
    Command command = { 0 };
    push_compiler(&command, build);
    if (sanitized(build)) {
        command_push(&command, "-fsanitize=address,undefined");
    }
    for (size_t i = 0; i < objects->count; i++) {
        command_push(&command, objects->items[i]);
    }
    command_push(&command, project);
    command_push(&command, libmc);
    command_push(&command, "-lpthread");
    command_push(&command, "-lm");
    command_push(&command, "-o");
    command_push(&command, binary);
    run_or_die(&command);
}

static void build_program(const Build *build, char *binary)
{
    char libmc[PATH_MAX_LEN];
    char project[PATH_MAX_LEN];
    build_libraries(build, libmc, project);
    FileList main_object = { 0 };
    object_path(main_object.items[main_object.count++], build->obj_dir, MAIN_SOURCE);
    compile(build, MAIN_SOURCE, main_object.items[0]);
    snprintf(binary, PATH_MAX_LEN, "%s/bprocs", build->out_dir);
    link_binary(build, &main_object, project, libmc, binary);
}

static int build_and_run_tests(const Build *build)
{
    char libmc[PATH_MAX_LEN];
    char project[PATH_MAX_LEN];
    build_libraries(build, libmc, project);
    Build tests = *build;
    snprintf(tests.obj_dir, sizeof tests.obj_dir, "%s/tests", build->obj_dir);
    mkdir_p(tests.obj_dir);
    FileList test_objects = { 0 };
    compile_directory(&tests, "tests", NULL, &test_objects);
    char binary[PATH_MAX_LEN];
    snprintf(binary, sizeof binary, "%s/tests", build->out_dir);
    link_binary(build, &test_objects, project, libmc, binary);
    Command run = { 0 };
    command_push(&run, binary);
    return command_run(&run);
}

static void configure(Build *build, bool release, const Target *target)
{
    build->release = release;
    build->dependency = false;
    build->target = target;
    build->headers_mtime = newest_header_mtime();
    snprintf(build->out_dir, sizeof build->out_dir, "out/%s%s%s", target != NULL ? target->name : "",
             target != NULL ? "/" : "", release ? "release" : "debug");
    snprintf(build->obj_dir, sizeof build->obj_dir, "%s/obj", build->out_dir);
}

static int run_cross(void)
{
    for (size_t i = 0; i < sizeof TARGETS / sizeof *TARGETS; i++) {
        Build build;
        configure(&build, true, &TARGETS[i]);
        char binary[PATH_MAX_LEN];
        build_program(&build, binary);
        printf("built %s\n", binary);
    }
    return 0;
}

static void rebuild_self_if_needed(char **argv)
{
    const char *source = "tools/build.c";
    if (mtime_of(source) <= mtime_of(argv[0])) {
        return;
    }
    printf("build: rebuilding self\n");
    Command command = { 0 };
    command_push(&command, "cc");
    command_push(&command, source);
    command_push(&command, "-o");
    command_push(&command, argv[0]);
    run_or_die(&command);
    execv(argv[0], argv);
    fprintf(stderr, "build: exec failed: %s\n", strerror(errno));
    exit(1);
}

// installed_paths writes $DESTDIR$PREFIX/bin and the binary's path in it.
// PREFIX defaults to ~/.local; DESTDIR stages the install for packaging.
static void installed_paths(char *bin_dir, char *binary)
{
    const char *destdir = getenv("DESTDIR");
    const char *prefix = getenv("PREFIX");
    char default_prefix[PATH_MAX_LEN];
    if (prefix == NULL || prefix[0] == '\0') {
        const char *home = getenv("HOME");
        if (home == NULL || home[0] == '\0') {
            fprintf(stderr, "build: set PREFIX or HOME to install\n");
            exit(2);
        }
        snprintf(default_prefix, sizeof default_prefix, "%s/.local", home);
        prefix = default_prefix;
    }
    snprintf(bin_dir, PATH_MAX_LEN, "%s%s/bin", destdir != NULL ? destdir : "", prefix);
    snprintf(binary, PATH_MAX_LEN, "%s/bprocs", bin_dir);
}

static int run_install(void)
{
    Build build;
    configure(&build, true, NULL);
    char built[PATH_MAX_LEN];
    build_program(&build, built);
    char bin_dir[PATH_MAX_LEN];
    char installed[PATH_MAX_LEN];
    installed_paths(bin_dir, installed);
    Command make_dir = { 0 };
    command_push(&make_dir, "install");
    command_push(&make_dir, "-d");
    command_push(&make_dir, bin_dir);
    run_or_die(&make_dir);
    Command copy = { 0 };
    command_push(&copy, "install");
    command_push(&copy, "-m");
    command_push(&copy, "755");
    command_push(&copy, built);
    command_push(&copy, installed);
    run_or_die(&copy);
    printf("installed %s\n", installed);
    return 0;
}

static int run_uninstall(void)
{
    char bin_dir[PATH_MAX_LEN];
    char installed[PATH_MAX_LEN];
    installed_paths(bin_dir, installed);
    if (access(installed, F_OK) != 0) {
        printf("nothing installed at %s\n", installed);
        return 0;
    }
    Command command = { 0 };
    command_push(&command, "rm");
    command_push(&command, "-f");
    command_push(&command, installed);
    run_or_die(&command);
    printf("removed %s\n", installed);
    return 0;
}

static int run_check(void)
{
    const char *home = getenv("HOME");
    char checker[PATH_MAX_LEN];
    snprintf(checker, sizeof checker, "%s/.agents/tools/modern-c/modern-c", home == NULL ? "" : home);
    Command command = { 0 };
    command_push(&command, "ruby");
    command_push(&command, checker);
    command_push(&command, "check");
    command_push(&command, ".");
    return command_run(&command);
}

int main(int argc, char **argv)
{
    rebuild_self_if_needed(argv);
    const char *mode = argc > 1 ? argv[1] : "build";

    if (strcmp(mode, "clean") == 0) {
        Command command = { 0 };
        command_push(&command, "rm");
        command_push(&command, "-rf");
        command_push(&command, "out");
        return command_run(&command);
    }
    if (strcmp(mode, "cross") == 0) {
        return run_cross();
    }
    if (strcmp(mode, "install") == 0) {
        return run_install();
    }
    if (strcmp(mode, "uninstall") == 0) {
        return run_uninstall();
    }

    Build build;
    configure(&build, strcmp(mode, "release") == 0, NULL);
    if (strcmp(mode, "build") == 0 || strcmp(mode, "release") == 0) {
        char binary[PATH_MAX_LEN];
        build_program(&build, binary);
        printf("built %s\n", binary);
        return 0;
    }
    if (strcmp(mode, "test") == 0) {
        return build_and_run_tests(&build);
    }
    if (strcmp(mode, "check") == 0) {
        int status = run_check();
        return status != 0 ? status : build_and_run_tests(&build);
    }

    fprintf(stderr, "build: unknown mode '%s'\nmodes: build, release, test, check, cross, install, uninstall, clean\n", mode);
    return 2;
}
