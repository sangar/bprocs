#include "userconfig.h"

#include "layout.h"
#include "mc/encoding/yaml.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"

static const struct {
    LayoutBox box;
    const char *name;
} BOX_NAMES[] = {
    { BOX_CPU, "cpu" },
    { BOX_MEMORY, "mem" },
    { BOX_NETWORK, "net" },
    { BOX_DETAIL, "details" },
};

String user_config_path(Arena *arena)
{
    String home;
    // The XDG spec says to ignore a relative $XDG_CONFIG_HOME.
    if (!env_get(arena, S("XDG_CONFIG_HOME"), &home) || !path_is_absolute(home)) {
        home = path_join(arena, env_home(arena), S(".config"));
    }
    return path_join(arena, home, S("bprocs/config.yml"));
}

static bool box_named(String name, unsigned *box)
{
    for (size_t i = 0; i < countof(BOX_NAMES); i++) {
        if (str_equal(name, S(BOX_NAMES[i].name))) {
            *box = BOX_NAMES[i].box;
            return true;
        }
    }
    return false;
}

[[nodiscard]] static Error read_boxes(Arena *arena, const Node *node, unsigned *boxes, Err *err)
{
    if (node->kind == NODE_NULL) {
        *boxes = 0;
        return ERR_OK;
    }
    if (node->kind != NODE_STRING) {
        return err_set(err, ERR_PARSE, "line %zu: boxes: expected names, like cpu mem net details", node->line);
    }
    StringList names = str_fields(arena, node->text);
    *boxes = 0;
    for (size_t i = 0; i < names.count; i++) {
        unsigned box;
        if (!box_named(names.items[i], &box)) {
            return err_set(err, ERR_PARSE, "line %zu: boxes: no box %.*s; they are cpu, mem, net and details",
                           node->line, (int)names.items[i].len, names.items[i].data);
        }
        *boxes |= box;
    }
    return ERR_OK;
}

[[nodiscard]] static Error read_user_config(Arena *arena, String text, UserConfig *config, Err *err)
{
    Node *root;
    Error parsed = yaml_parse(arena, text, &root, err);
    if (parsed != ERR_OK || root->kind == NODE_NULL) {
        return parsed;
    }
    if (root->kind != NODE_MAPPING) {
        return err_set(err, ERR_PARSE, "line %zu: expected settings, like boxes: cpu mem", root->line);
    }
    for (size_t i = 0; i < root->entry_count; i++) {
        String key = root->entries[i].key;
        const Node *value = root->entries[i].value;
        Error e = str_equal(key, S("boxes")) ? read_boxes(arena, value, &config->boxes, err)
                                             : err_set(err, ERR_PARSE, "line %zu: %.*s: unknown setting", value->line,
                                                       (int)key.len, key.data);
        if (e != ERR_OK) {
            return e;
        }
    }
    return ERR_OK;
}

Error user_config_load(Arena *arena, String path, UserConfig *config, Err *err)
{
    *config = (UserConfig) { .path = path, .boxes = BOX_ALL };
    if (!file_exists(path)) {
        return ERR_OK;
    }
    String text;
    Error e = file_read_all(arena, path, &text, err);
    if (e == ERR_OK) {
        e = read_user_config(arena, text, config, err);
    }
    return e == ERR_OK ? ERR_OK : err_wrap(err, e, "%.*s", (int)path.len, path.data);
}

static String boxes_line(Arena *arena, unsigned boxes)
{
    StringList names = { 0 };
    for (size_t i = 0; i < countof(BOX_NAMES); i++) {
        if (boxes & BOX_NAMES[i].box) {
            strlist_push(arena, &names, S(BOX_NAMES[i].name));
        }
    }
    return names.count == 0 ? S("boxes: \"\"") : str_concat(arena, S("boxes: "), str_join(arena, names, S(" ")));
}

Error user_config_save_boxes(Arena *arena, String path, unsigned boxes, Err *err)
{
    String text = S("");
    if (file_exists(path)) {
        Error read = file_read_all(arena, path, &text, err);
        if (read != ERR_OK) {
            return read;
        }
    }
    StringList lines = strlist_from_lines(arena, str_trim_suffix(text, S("\n")));
    String line = boxes_line(arena, boxes);
    bool replaced = false;
    for (size_t i = 0; i < lines.count; i++) {
        if (str_starts_with(lines.items[i], S("boxes:"))) {
            lines.items[i] = line;
            replaced = true;
        }
    }
    if (!replaced) {
        strlist_push(arena, &lines, line);
    }
    String written = str_concat(arena, str_join(arena, lines, S("\n")), S("\n"));
    return file_write_atomic(path, written, 0644, err);
}
