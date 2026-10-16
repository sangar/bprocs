#include "test.h"

#include "config.h"
#include "mc/platform/platform.h"
#include "mc/text/path.h"

static const char CONFIG[] = "theme: nord\n"
                             "env: { RAILS_ENV: development, SHARED: top }\n"
                             "log_dir: var/logs\n"
                             "procs:\n"
                             "  db:\n"
                             "    run: postgres -D .data/db\n"
                             "    port: 5432\n"
                             "    ready: { port: 5432, timeout: 30s }\n"
                             "  migrate:\n"
                             "    run: [bin/rails, db:migrate]\n"
                             "    job: true\n"
                             "    needs: [db]\n"
                             "  web:\n"
                             "    run: |\n"
                             "      bin/rails server\n"
                             "      echo done\n"
                             "    cwd: app\n"
                             "    port: 3000\n"
                             "    env:\n"
                             "      SHARED: mine\n"
                             "      UNSET: null\n"
                             "    needs: [db, migrate]\n"
                             "    ready: { http: /up, every: 500ms }\n"
                             "    health: { cmd: [curl, -f, localhost:3000], every: 10s, failures: 2 }\n"
                             "    restart: on-failure\n"
                             "    stop: { signal: SIGINT, timeout: 10s }\n"
                             "    log: true\n"
                             "  worker:\n"
                             "    run: bundle exec sidekiq\n"
                             "    cwd: /srv/worker\n"
                             "    autostart: false\n"
                             "    restart: always\n"
                             "    ready: { log: Started }\n"
                             "    health: { http: \"http://127.0.0.1:9000/health\" }\n"
                             "    log: log/worker.txt\n"
                             "    color: \"#ff8800\"\n";

[[nodiscard]] static Error parse(Test *test, const char *text, Config *config, Err *err)
{
    return config_parse(test->arena, S(text), S("/home/me/bprocs.yml"), config, err);
}

static void config_reads_processes_in_order(Test *test)
{
    Config config;
    Err err = { 0 };
    test_check(test, parse(test, CONFIG, &config, &err) == ERR_OK, err.msg);
    test_check(test, config.proc_count == 4, "four processes");
    if (config.proc_count != 4) {
        return;
    }
    test_check_str(test, config.theme, S("nord"), "the theme");
    const ProcSpec *db = &config.procs[0];
    const ProcSpec *migrate = &config.procs[1];
    const ProcSpec *web = &config.procs[2];
    const ProcSpec *worker = &config.procs[3];
    test_check_str(test, web->name, S("web"), "in the order written");
    test_check(test, web->argv.count == 3 && str_equal(web->argv.items[0], S("/bin/sh")), "text runs in sh -c");
    test_check_str(test, web->command, S("bin/rails server …"), "a script shows by its first line");
    test_check(test, migrate->argv.count == 2 && str_equal(migrate->argv.items[1], S("db:migrate")), "a list is argv");
    test_check_str(test, migrate->command, S("bin/rails db:migrate"), "words show joined");
    test_check_str(test, web->cwd, S("/home/me/app"), "cwd is relative to the config");
    test_check_str(test, worker->cwd, S("/srv/worker"), "an absolute cwd stays");
    test_check_str(test, db->cwd, S("/home/me"), "the default cwd is the config's directory");

    test_check(test, web->env.count == 5, "the top level's env, the port, then the proc's");
    if (web->env.count == 5) {
        test_check_str(test, web->env.items[0], S("RAILS_ENV=development"), "top-level env first");
        test_check_str(test, web->env.items[2], S("PORT=3000"), "the port sets PORT");
        test_check_str(test, web->env.items[3], S("SHARED=mine"), "the proc's env comes last, so it wins");
        test_check_str(test, web->env.items[4], S("UNSET"), "null removes a variable");
    }

    test_check(test, migrate->job && migrate->need_count == 1 && migrate->needs[0] == 0, "a job that needs db");
    test_check(test, web->need_count == 2 && web->needs[0] == 0 && web->needs[1] == 1, "needs are indexes");
    test_check(test, db->ready.kind == CHECK_PORT && db->ready.port == 5432 && db->ready.timeout_ns == (int64_t)30 * NS_PER_SECOND,
               "a port check with a timeout");
    test_check(test, web->ready.kind == CHECK_HTTP && web->ready.port == 3000 && str_equal(web->ready.path, S("/up")) &&
                         str_equal(web->ready.host, S("localhost")) && web->ready.every_ns == (int64_t)500 * NS_PER_MILLISECOND,
               "a bare path uses the proc's port");
    test_check(test, web->health.kind == CHECK_CMD && web->health.argv.count == 3 && web->health.failures == 2,
               "a health command");
    test_check(test, worker->ready.kind == CHECK_LOG && str_equal(worker->ready.text, S("Started")), "a log check");
    test_check(test, worker->health.kind == CHECK_HTTP && worker->health.port == 9000 &&
                         str_equal(worker->health.host, S("127.0.0.1")) &&
                         str_equal(worker->health.path, S("/health")) && worker->health.failures == 3,
               "a whole URL, and health's defaults");

    test_check(test, web->restart == RESTART_ON_FAILURE && worker->restart == RESTART_ALWAYS &&
                         db->restart == RESTART_NO,
               "restart policies");
    test_check(test, web->stop == PTY_INTERRUPT && web->stop_timeout_ns == (int64_t)10 * NS_PER_SECOND, "stop with a timeout");
    test_check(test, db->stop == PTY_TERMINATE && db->stop_timeout_ns == (int64_t)5 * NS_PER_SECOND, "stop's defaults");
    test_check_str(test, web->log_path, S("/home/me/var/logs/web.log"), "log: true writes into log_dir");
    test_check_str(test, worker->log_path, S("/home/me/log/worker.txt"), "or to the path given");
    test_check_str(test, db->log_path, S(""), "no log by default");
    test_check(test, db->autostart && !worker->autostart, "autostart");
    test_check(test, worker->has_color && rgb_equal(worker->color, (Rgb) { 0xff, 0x88, 0x00 }), "colour");
    size_t index;
    test_check(test, config_proc_index(&config, S("worker"), &index) && index == 3, "procs are found by name");
    Error parsed = parse(test, "procs:\n  a: { run: x, stop: SIGHUP }\n  b: { run: x, stop: { signal: SIGQUIT } }\n",
                         &config, &err);
    test_check(test, parsed == ERR_OK && config.procs[0].stop == PTY_RELOAD && config.procs[1].stop == PTY_QUIT,
               "stop takes SIGHUP and SIGQUIT too");
}

static void config_reads_env_files(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String dir;
    test_check(test, dir_create_temp(a, S("bprocs-test-"), &dir, &err) == ERR_OK, "a temp dir");
    test_check(test, file_write_all(path_join(a, dir, S(".env")), S("SHARED=file\nTOP=1\n"), 0600, &err) == ERR_OK,
               "write .env");
    test_check(test, file_write_all(path_join(a, dir, S("web.env")), S("export WEB='yes'\n"), 0600, &err) == ERR_OK,
               "write web.env");
    Config config;
    String text = S("env_file: .env\n"
                    "env: { SHARED: env }\n"
                    "procs:\n"
                    "  web: { run: x, env_file: [web.env], env: { OWN: 1 } }\n");
    Error parsed = config_parse(a, text, path_join(a, dir, S("bprocs.yml")), &config, &err);
    test_check(test, parsed == ERR_OK, err.msg);
    if (parsed == ERR_OK) {
        StringList env = config.procs[0].env;
        test_check(test, env.count == 5, "files, then env, at each level");
        test_check(test, env.count == 5 && str_equal(env.items[2], S("SHARED=env")) &&
                             str_equal(env.items[3], S("WEB=yes")) && str_equal(env.items[4], S("OWN=1")),
                   "in the order that lets env: win over a file");
    }
    text = S("env_file: missing.env\nprocs: { web: { run: x } }\n");
    parsed = config_parse(a, text, path_join(a, dir, S("bprocs.yml")), &config, &err);
    test_check(test, parsed == ERR_PARSE && str_starts_with(S(err.msg), S("line 1: env_file: ")), "a missing file");
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "clean up");
}

static void config_reads_a_procfile_as_foreman_does(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String dir;
    test_check(test, dir_create_temp(a, S("bprocs-test-"), &dir, &err) == ERR_OK, "a temp dir");
    test_check(test, file_write_all(path_join(a, dir, S(".env")), S("PORT=3000\nRAILS_ENV=development\n"), 0600, &err) ==
                         ERR_OK,
               "write .env");
    String text = S("# the app\n"
                    "web: bin/rails server -p $PORT\n"
                    "\n"
                    "Worker_2:   bundle exec sidekiq\r\n");
    Config config;
    Error parsed = config_parse(a, text, path_join(a, dir, S("Procfile.dev")), &config, &err);
    test_check(test, parsed == ERR_OK && config.proc_count == 2, err.msg);
    if (parsed == ERR_OK && config.proc_count == 2) {
        const ProcSpec *web = &config.procs[0];
        const ProcSpec *worker = &config.procs[1];
        test_check_str(test, worker->name, S("Worker_2"), "foreman's names, capitals and all");
        test_check(test, web->argv.count == 3 && str_equal(web->argv.items[0], S("/bin/sh")) &&
                             str_equal(web->argv.items[2], S("bin/rails server -p $PORT")),
                   "a command runs in sh -c");
        test_check_str(test, worker->command, S("bundle exec sidekiq"), "the command as written");
        test_check_str(test, web->cwd, dir, "in the Procfile's directory");
        test_check(test, web->env.count == 3 && str_equal(web->env.items[1], S("RAILS_ENV=development")),
                   "with the .env beside it");
        test_check_str(test, web->env.items[web->env.count - 1], S("PORT=3000"), "PORT starts at the .env's PORT");
        test_check_str(test, worker->env.items[worker->env.count - 1], S("PORT=3100"), "and goes up by 100");
        test_check(test, web->port == 0, "PORT is not the proc's port");
    }
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "clean up");

    parsed = config_parse(a, S("web: x\n"), S("/home/me/Procfile"), &config, &err);
    test_check(test, parsed == ERR_OK && str_equal(config.procs[0].env.items[0], S("PORT=5000")),
               "without a .env PORT starts at 5000");
    static const struct {
        const char *text;
        const char *message;
    } CASES[] = {
        { "web bin/rails server\n", "line 1: expected name: command, such as web: bin/rails server" },
        { "web: a\n\nweb: b\n", "line 3: web: there is already a proc called web" },
        { "# nothing yet\n", "no lines such as web: bin/rails server say what to run" },
    };
    for (size_t i = 0; i < countof(CASES); i++) {
        parsed = config_parse(a, S(CASES[i].text), S("/home/me/Procfile"), &config, &err);
        test_check(test, parsed == ERR_PARSE, CASES[i].message);
        test_check_str(test, S(err.msg), S(CASES[i].message), "the message");
    }
}

static void config_says_where_it_is_wrong(Test *test)
{
    static const struct {
        const char *text;
        const char *message;
    } CASES[] = {
        { "procs:\n  web:\n    run: rails s\n    autostrat: true\n", "line 4: procs.web.autostrat: unknown setting" },
        { "procs:\n  web:\n    cwd: x\n", "line 3: procs.web: needs run: to say what to run" },
        { "theme: nord\n", "no procs: section says what to run" },
        { "procs:\n  web:\n    run: x\n    needs: [db]\n", "line 4: procs.web.needs: no proc called db" },
        { "procs:\n  a: { run: x, needs: [b] }\n  b: { run: x, needs: [c] }\n  c: { run: x, needs: [b] }\n",
          "line 3: procs.b.needs: needs go round in a loop: b → c → b" },
        { "procs:\n  web: { run: x, ready: { http: /up } }\n",
          "line 2: procs.web.ready: a path needs the proc's port: set port:, or give a whole URL" },
        { "procs:\n  web: { run: x, ready: { http: \"https://a/\" } }\n",
          "line 2: procs.web.ready: expected a path such as /up, or a URL such as http://localhost:3000/up" },
        { "procs:\n  m:\n    run: x\n    job: true\n    restart: always\n",
          "line 5: procs.m: a job cannot restart: always" },
        { "procs:\n  m:\n    run: x\n    job: true\n    ready: { log: ok }\n",
          "line 5: procs.m: a job is done when it exits, so it takes no ready:" },
        { "procs:\n  a: { run: x, port: 3000 }\n  b:\n    run: x\n    env: { PORT: 3000 }\n",
          "line 4: procs.b: port 3000 is also the port of a" },
        { "procs:\n  Web: { run: x }\n", "line 2: procs.Web: a name is lowercase letters, digits, - and _" },
        { "procs:\n  web: { run: x, ready: { log: a, port: 1 } }\n",
          "line 2: procs.web.ready.port: a check has one of log, port, http or cmd" },
        { "procs:\n  web: { run: x, health: { log: a } }\n", "line 2: procs.web.health: health takes port, http or cmd" },
        { "procs:\n  web: { run: x, restart: sometimes }\n",
          "line 2: procs.web.restart: expected no, on-failure or always" },
        { "procs:\n  web: { run: x, stop: { timeout: soon } }\n",
          "line 2: procs.web.stop.timeout: expected a duration such as 500ms, 10s or 2m" },
    };
    for (size_t i = 0; i < countof(CASES); i++) {
        Config config;
        Err err = { 0 };
        Error parsed = parse(test, CASES[i].text, &config, &err);
        test_check(test, parsed == ERR_PARSE, CASES[i].message);
        test_check_str(test, S(err.msg), S(CASES[i].message), "the message");
    }
}

static void config_is_found_above_the_working_directory(Test *test)
{
    Arena *a = test->arena;
    Err err = { 0 };
    String dir;
    test_check(test, dir_create_temp(a, S("bprocs-test-"), &dir, &err) == ERR_OK, "a temp dir");
    String deep = path_join(a, dir, S("app/models"));
    test_check(test, dir_create_all(deep, 0700, &err) == ERR_OK, "make directories");
    test_check(test, file_write_all(path_join(a, dir, S("bprocs.yml")), S("procs: {}\n"), 0600, &err) == ERR_OK,
               "write a config");
    String found;
    test_check(test, config_find(a, deep, &found), "found from below");
    test_check(test, str_ends_with(found, S("/bprocs.yml")) && str_starts_with(found, dir), "the one above");
    String app = path_join(a, dir, S("app"));
    test_check(test, file_write_all(path_join(a, app, S("Procfile")), S("web: x\n"), 0600, &err) == ERR_OK &&
                         file_write_all(path_join(a, app, S("Procfile.dev")), S("web: x\n"), 0600, &err) == ERR_OK,
               "write Procfiles nearer");
    test_check(test, config_find(a, deep, &found), "found again");
    test_check_str(test, found, path_join(a, app, S("Procfile.dev")), "the nearest, and Procfile.dev before Procfile");
    test_check(test, dir_remove_all(dir, &err) == ERR_OK, "clean up");
}

static void config_examples_are_valid(Test *test)
{
    static const struct {
        const char *path;
        size_t procs;
    } EXAMPLES[] = {
        { "examples/minimal/bprocs.yml", 2 },
        { "examples/medium/bprocs.yml", 7 },
        { "examples/max/bprocs.yml", 9 },
        { "examples/procfile/Procfile", 3 },
    };
    for (size_t i = 0; i < countof(EXAMPLES); i++) {
        Config config;
        Err err = { 0 };
        Error loaded = config_load(test->arena, S(EXAMPLES[i].path), &config, &err);
        test_check(test, loaded == ERR_OK, err.msg);
        test_check(test, loaded != ERR_OK || config.proc_count == EXAMPLES[i].procs, EXAMPLES[i].path);
    }
}

const TestCase CONFIG_TESTS[] = {
    { "config_reads_processes_in_order", config_reads_processes_in_order },
    { "config_reads_env_files", config_reads_env_files },
    { "config_reads_a_procfile_as_foreman_does", config_reads_a_procfile_as_foreman_does },
    { "config_says_where_it_is_wrong", config_says_where_it_is_wrong },
    { "config_is_found_above_the_working_directory", config_is_found_above_the_working_directory },
    { "config_examples_are_valid", config_examples_are_valid },
    { nullptr, nullptr },
};
