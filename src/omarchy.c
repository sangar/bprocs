#include "omarchy.h"

#include "mc/platform/platform.h"
#include "mc/text/path.h"
#include "userconfig.h"

static const char TEMPLATE[] =
    "# Made by bprocs setup omarchy. Omarchy makes bprocs-theme.yml of it on every theme change.\n"
    "background: \"{{ background }}\"\n"
    "surface: \"{{ lighter_background }}\"\n"
    "border: \"{{ selection }}\"\n"
    "focus: \"{{ accent }}\"\n"
    "text: \"{{ foreground }}\"\n"
    "muted: \"{{ muted }}\"\n"
    "faint: \"{{ lighter_background }}\"\n"
    "highlight: \"{{ orange }}\"\n"
    "selection: \"{{ selection }}\"\n"
    "running: \"{{ green }}\"\n"
    "starting: \"{{ yellow }}\"\n"
    "stopped: \"{{ muted }}\"\n"
    "failed: \"{{ red }}\"\n"
    "cpu: [\"{{ green }}\", \"{{ yellow }}\", \"{{ red }}\"]\n"
    "memory: [\"{{ cyan }}\", \"{{ blue }}\", \"{{ magenta }}\"]\n"
    "download: [\"{{ cyan }}\", \"{{ bright_cyan }}\", \"{{ foreground }}\"]\n"
    "upload: [\"{{ magenta }}\", \"{{ bright_magenta }}\", \"{{ red }}\"]\n"
    "accents: [\"{{ blue }}\", \"{{ magenta }}\", \"{{ cyan }}\", \"{{ green }}\",\n"
    "          \"{{ yellow }}\", \"{{ orange }}\", \"{{ red }}\", \"{{ bright_cyan }}\"]\n"
    "# As Omarchy's alacritty.toml maps them.\n"
    "ansi: [\"{{ background }}\", \"{{ red }}\", \"{{ green }}\", \"{{ yellow }}\",\n"
    "       \"{{ blue }}\", \"{{ magenta }}\", \"{{ cyan }}\", \"{{ foreground }}\",\n"
    "       \"{{ muted }}\", \"{{ bright_red }}\", \"{{ bright_green }}\", \"{{ bright_yellow }}\",\n"
    "       \"{{ bright_blue }}\", \"{{ bright_magenta }}\", \"{{ bright_cyan }}\", \"{{ bright_foreground }}\"]\n";

[[nodiscard]] static Error run(Arena *arena, StringList argv, Err *err)
{
    ProcessResult result;
    Error e = process_run(arena, argv, S(""), &result, err);
    if (e == ERR_OK && result.exit_code != 0) {
        return err_set(err, ERR_PLATFORM, "%.*s failed: %.*s", (int)argv.items[0].len, argv.items[0].data,
                       (int)str_trim(result.stderr_text).len, str_trim(result.stderr_text).data);
    }
    return e;
}

Error omarchy_setup(Arena *arena, Err *err)
{
    String home = env_home(arena);
    String omarchy = path_join(arena, home, S(".config/omarchy"));
    if (!file_exists(omarchy)) {
        return err_set(err, ERR_NOT_FOUND, "no Omarchy here: %.*s is missing", (int)omarchy.len, omarchy.data);
    }
    String generated = path_join(arena, home, S(".local/state/omarchy/current/theme/bprocs-theme.yml"));
    String link = user_theme_path(arena, S("omarchy"));
    // libmc makes no symlinks, so ln does.
    StringList ln = { 0 };
    strlist_push(arena, &ln, S("ln"));
    strlist_push(arena, &ln, S("-snf"));
    strlist_push(arena, &ln, generated);
    strlist_push(arena, &ln, link);
    StringList refresh = { 0 };
    strlist_push(arena, &refresh, S("omarchy-theme-refresh"));
    Error e = file_write_atomic(path_join(arena, omarchy, S("themed/bprocs-theme.yml.tpl")), S(TEMPLATE), 0644, err);
    if (e == ERR_OK) {
        e = dir_create_all(path_dir(link), 0755, err);
    }
    if (e == ERR_OK) {
        e = run(arena, ln, err);
    }
    if (e == ERR_OK) {
        e = user_config_save_theme(arena, user_config_path(arena), S("omarchy"), err);
    }
    if (e == ERR_OK) {
        e = run(arena, refresh, err);
    }
    return e;
}
