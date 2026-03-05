#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static bool is_separator(char c) {
    return c == '/' || c == '\\';
}

static void normalize_path(char *path) {
    size_t len = strlen(path);
    for (size_t i = 0; i < len; ++i) {
        if (path[i] == '\\') {
            path[i] = '/';
        }
    }
    while (len > 1 && is_separator(path[len - 1])) {
        path[len - 1] = '\0';
        --len;
    }
}

static int platform_mkdir(const char *path) {
#ifdef _WIN32
    char converted[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(converted)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    for (size_t i = 0; i <= len; ++i) {
        char c = path[i];
        if (c == '/') {
            c = '\\';
        }
        converted[i] = c;
    }
    if (_mkdir(converted) == 0) {
        return 0;
    }
    return -1;
#else
    return mkdir(path, 0777);
#endif
}

static int mkdir_p(const char *path) {
    if (!path || !*path) {
        errno = EINVAL;
        return -1;
    }
    char buffer[PATH_MAX];
    size_t len = strlen(path);
    if (len >= sizeof(buffer)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(buffer, path, len + 1);
    size_t offset = 1;
#ifdef _WIN32
    if (len >= 2 && buffer[1] == ':') {
        offset = 3; // skip the drive prefix (e.g., C:/)
    }
#endif
    for (char *p = buffer + offset; *p; ++p) {
        if (is_separator(*p)) {
            *p = '\0';
            if (platform_mkdir(buffer) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (platform_mkdir(buffer) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void derive_project_name(const char *path, char *buffer, size_t size) {
    size_t len = strlen(path);
    while (len > 0 && path[len - 1] == '/') {
        --len;
    }
    size_t start = len;
    while (start > 0 && path[start - 1] != '/') {
        --start;
    }
    size_t name_len = len > start ? len - start : 0;
    if (name_len == 0) {
        snprintf(buffer, size, "nova_project");
        return;
    }
    if (name_len >= size) {
        name_len = size - 1;
    }
    memcpy(buffer, path + start, name_len);
    buffer[name_len] = '\0';
}

static void sanitize_module_name(const char *name, char *buffer, size_t size) {
    size_t out = 0;
    for (size_t i = 0; name[i] != '\0' && out + 1 < size; ++i) {
        char c = name[i];
        if (isalnum((unsigned char)c)) {
            buffer[out++] = (char)tolower((unsigned char)c);
        } else if (c == '_' || c == '-') {
            buffer[out++] = '_';
        }
    }
    if (out == 0) {
        snprintf(buffer, size, "app");
        return;
    }
    buffer[out] = '\0';
}

static bool write_file(const char *path, const char *contents) {
    FILE *file = fopen(path, "w");
    if (!file) {
        return false;
    }
    size_t len = strlen(contents);
    size_t written = fwrite(contents, 1, len, file);
    fclose(file);
    return written == len;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <project-path>\n", argv[0]);
        return 1;
    }
    char project_root[PATH_MAX];
    if (strlen(argv[1]) >= sizeof(project_root)) {
        fprintf(stderr, "project path is too long\n");
        return 1;
    }
    strcpy(project_root, argv[1]);
    normalize_path(project_root);
    if (mkdir_p(project_root) != 0) {
        perror("failed to create project directory");
        return 1;
    }

    char project_name[256];
    derive_project_name(project_root, project_name, sizeof(project_name));

    char module_name[256];
    sanitize_module_name(project_name, module_name, sizeof(module_name));

    char src_dir[PATH_MAX];
    size_t root_len = strlen(project_root);
    if (root_len + 4 >= sizeof(src_dir)) {
        fprintf(stderr, "project path is too long\n");
        return 1;
    }
    memcpy(src_dir, project_root, root_len);
    memcpy(src_dir + root_len, "/src", 5);
    if (mkdir_p(src_dir) != 0) {
        perror("failed to create src directory");
        return 1;
    }

    char toml_path[PATH_MAX];
    if (root_len + 10 >= sizeof(toml_path)) {
        fprintf(stderr, "project path is too long\n");
        return 1;
    }
    memcpy(toml_path, project_root, root_len);
    memcpy(toml_path + root_len, "/nova.toml", 11);
    char toml_contents[512];
    snprintf(toml_contents, sizeof(toml_contents),
             "[project]\n"
             "name = \"%s\"\n"
             "version = \"0.1.0\"\n"
             "targets = [\"native\"]\n"
             "\n"
             "[build]\n"
             "entry = \"src/main.nova\"\n",
             project_name);
    if (!write_file(toml_path, toml_contents)) {
        perror("failed to write nova.toml");
        return 1;
    }

    char main_path[PATH_MAX];
    if (root_len + 14 >= sizeof(main_path)) {
        fprintf(stderr, "project path is too long\n");
        return 1;
    }
    memcpy(main_path, project_root, root_len);
    memcpy(main_path + root_len, "/src/main.nova", 15);
    char main_contents[1024];
    snprintf(main_contents, sizeof(main_contents),
             "module %s.main\n\n"
             "fun answer(): Number = 42\n\n"
             "fun main(): Number = if true { answer() } else { 0 }\n",
             module_name);
    if (!write_file(main_path, main_contents)) {
        perror("failed to write src/main.nova");
        return 1;
    }

    printf("Created Nova project '%s' in %s\n", project_name, project_root);
    return 0;
}
