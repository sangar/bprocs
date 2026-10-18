# Plan: apps with their own Procfile

Context: `make-app/bprocs.yml` has one proc per app. Each runs
`mise exec -- sh -c 'bin/setup --skip-server && bprocs up'`, which starts a second
daemon from the app's `Procfile`. The `mise exec -- sh -c` wrapper is there so
that both commands get the app's Ruby version and not the one bprocs was started
with. Four things go wrong with that.

1. **The outer tab hides failures.** `bprocs up` returns, so the tab shows a clean
   exit while the app's `web` has crashed. The state is only visible with
   `bprocs ps` run in each app. A `health:` check cannot fix this: health checks
   only run while the proc is `running` (`src/supervisor.c:519`), and the outer
   proc has exited.
2. **The inner daemons outlive the outer one.** They are detached, so an outer
   `bprocs down` leaves every app running. The next start finds the old daemon,
   and `bprocs run` for that app refuses to start because the old daemon holds
   the lock.
3. **`PORT` from the environment is ignored for a Procfile.** `read_procfile`
   (`src/config.c:725`) takes `PORT` from the `.env` beside the Procfile, else
   5000, and appends `PORT=n` after the inherited environment, so it always wins.
   `PORT: "3001"` in the outer `env:` never reaches puma. `make-start` then binds
   5000, which macOS ControlCenter holds, and exits with `EADDRINUSE`. Every app
   would also collide on 5000 with the others.
4. **Procfile procs have no port, checks or needs.** The list shows no port, and
   nothing says an app is up.

## Phase 1: small fixes, no new concepts

Status: done in bprocs. The switch in `make-app/bprocs.yml` is still to do.

- **Done: `PORT` precedence for a Procfile:** `.env` `PORT`, then the inherited
  `PORT`, then 5000, which is foreman's order.
  - `config_load` reads `PORT` from the environment and passes it to
    `config_parse` as `inherited_port`, so `config_parse` stays a function of
    its arguments and the tests need no `setenv`. `read_procfile` uses it when
    the `.env` sets no `PORT`.
  - The `PORT` that counts is the daemon's, fixed when it started: a
    `bprocs reload` after changing `PORT` in the shell does not see the change,
    and neither does an inner daemon that already ran. The README says so.
  - Test: in `tests/test_config.c`, a Procfile loaded with `PORT` 3001 inherited
    gives `PORT=3001` for the first proc and 3101 for the second. A `.env`
    `PORT` still wins.
- **To do in `make-app`: run the inner config in the foreground.** Each outer proc runs
  `mise exec -- sh -c 'bin/setup --skip-server && bprocs run --exit-on-failure'`
  instead of `bprocs up`. No code change:
  - The outer proc keeps running, and its tab shows the inner lines with each
    proc's name in front.
  - It exits with 1 once an inner proc fails with no restart coming
    (`src/runner.c:133`), so the outer tab turns red.
  - It stops everything on `SIGTERM`, so an outer `bprocs down` stops the apps.
  - It still serves the app's socket, so `bprocs -C make-start ps`, `logs` and
    `attach` keep working.
  - Before switching, `bprocs -C <app> down` each app once to stop the leftover
    daemons.
  - Each outer proc sets its app's `PORT` in `env:`, such as `PORT: "3001"`,
    which now reaches the Procfile.
- **Done: `bprocs ps --fail`**, for CI: exit 1 when any proc failed with no
  restart coming, else 0. The rule is `proc_status_failed` (`src/proc.c`),
  which `bprocs run --exit-on-failure` now uses too. `run_ps` parses its flags
  in a loop, so `ps --json --fail` works and an unknown flag is an error.
  - Test: `proc_status_failed_only_without_a_restart_coming` in
    `tests/test_proc.c`.

Tried by hand on a Procfile with `PORT=3001`: `bprocs run --exit-on-failure`
gave 3001 and 3101 and exited with 1 when a proc exited with 3, and
`ps --fail` exited with 1 with a crashed proc and 0 with all running.

Once `make-app` switches, Phase 1 makes make-start start, shows inner crashes in
the outer tabs, and makes `bprocs down` stop everything.

## Phase 2: `include:`, which replaces the nested daemons

Status: done in bprocs, with `include_defaults:` for repeated settings (see
the open questions). The switch in `make-app/bprocs.yml` is still to do.

One daemon, one list, every app's processes in it.

```yaml
include:
  make-start:
    dir: make-start          # where the Procfile or bprocs.yml is; default: the key
    port: 3001               # PORT of its first proc, as above; Procfile only
    needs: [db, redis]       # top-level procs this app needs, optional
    setup: bin/setup --skip-server   # optional job, run before the others in this app
    env: { RUBY_DEBUG_OPEN: "true" } # added to every proc, optional
    wrap: [mise, exec, --]           # prefix for every command, optional
```

Rules:
- Procs are named `make-start:web`, `make-start:worker` and so on, with `cwd` the
  app directory and the app's `.env` read as it is for a lone Procfile. The
  include key is a name as `is_name` defines it.
- `setup:` becomes the job `make-start:setup`. Every other proc in that app
  gets `needs: [make-start:setup]`, so the migration and bundle output shows in
  its own tab and a failure blocks the app.
- `needs:` names top-level procs, such as a shared `db` or `redis`. It goes on
  `make-start:setup` when there is one, else on every proc in the app. Inside
  an included `bprocs.yml`, `needs` names stay inside the app.
- `wrap:` goes in front of every command in the app, `setup:` included, and
  replaces the `mise exec -- sh -c` around two commands. It does not wrap
  `cmd:` checks.
- The environment of an included proc is, later entries winning: the outer
  top-level `env_file:` and `env:`, the app's `.env`, `PORT=n`, the include's
  `env:`. The base port is the include's `port:`, else the `.env`'s `PORT`, else
  5000.
- An included `bprocs.yml` is read as a normal config: its `env_file:` and `env:`
  come after the app's `.env`, its paths and `log_dir` are relative to the app
  directory, and its `theme` and `scrollback` are errors. `port:` on such an
  include is an error, since its procs set their own.
- Includes inside an include are an error.
- Ports are checked across the whole set. The existing check (`check_ports`,
  `src/config.c:615`) only sees `port:` settings, and Procfile procs have none,
  so add a check on the `PORT` each Procfile proc gets: app A at 3000 gives its
  second proc 3100, which collides with an app B at 3100.
- `autostart: false` on an include entry leaves all its procs stopped,
  for `make-browsershots` and `make-integration-mystore`.
- `bprocs start make-start` (and `stop`, `restart`) acts on every proc of the
  app.
- `bprocs reload` reads the included files again and restarts only the procs
  that changed. `config_spec_runs_same` compares by name, so this needs nothing
  new.
- The TUI's "changed · R reloads it" watches only `config_path`
  (`src/app.c:638`). It also watches every included Procfile, `bprocs.yml`
  and `.env`.
- Errors in an included file name that file: `make-start/Procfile: line 3: …`.
- Running `bprocs` inside `make-app/make-start` finds `make-start/Procfile`
  before `make-app/bprocs.yml` and starts a second daemon for the same app,
  whose ports then collide. The README says to run from `make-app` or with
  `-C ..`.
- The README's config section and `examples/include/` get the new keys. Add
  tests for parsing, namespacing, `setup:` and `needs:` on an include, the
  environment order, port collisions across apps and unknown keys (they are
  errors with a line, as elsewhere).

### Example

`make-app/bprocs.yml` with shared services and three apps. `db` and `redis`
stand in for whatever the apps share.

```yaml
theme: tokyo-night
env: { RAILS_ENV: development }       # every proc, included ones too, gets this first

procs:                                 # shared services, run once for every app
  db:
    run: postgres -D .data/db
    port: 5432
    ready: { port: 5432 }
  redis:
    run: redis-server --port 6379
    port: 6379
    ready: { port: 6379 }

include:
  make-start:                          # dir defaults to make-start/
    port: 3001                         # web gets PORT=3001, worker 3101, ...
    needs: [db, redis]                 # goes on make-start:setup
    setup: bin/setup --skip-server     # the job make-start:setup; the rest need it
    wrap: [mise, exec, --]             # the app's Ruby, for setup and every proc
    env: { RUBY_DEBUG_OPEN: "true" }

  make-browsershots:
    port: 3002
    needs: [db]
    setup: bin/setup --skip-server
    wrap: [mise, exec, --]
    autostart: false                   # stays stopped until started

  make-integration-mystore:
    dir: integrations/mystore          # when the directory isn't the key
    port: 3003
    needs: [db, redis]
    setup: bin/setup --skip-server
    wrap: [mise, exec, --]
    autostart: false
```

With `web` and `worker` in `make-start/Procfile`, the list is:

```
db
redis
make-start:setup              job, needs db and redis
make-start:web                PORT=3001, needs make-start:setup
make-start:worker             PORT=3101, needs make-start:setup
make-browsershots:setup       stopped
make-browsershots:web         PORT=3002, stopped
make-integration-mystore:...  stopped
```

`make-start:web` runs `mise exec -- /bin/sh -c 'bin/rails server -p $PORT'` in
`make-start/`, with the top-level `env:`, then `make-start/.env`, then
`PORT=3001`, then the include's `env:`.

An app that needs checks, restarts or needs of its own has a `bprocs.yml`, which
is read instead of its Procfile, and its include entry takes no `port:`:

```yaml
# make-start/bprocs.yml
procs:
  web:
    run: bin/rails server -p 3001
    port: 3001
    ready: { http: /up }
    restart: on-failure
  worker:
    run: bundle exec sidekiq
    needs: [web]                       # names inside the app; becomes make-start:web
```

`make-browsershots` at `port: 3101` is an error, since that is
`make-start:worker`'s `PORT`. `bprocs start make-browsershots` starts its setup,
then the rest of it.

### Open questions

- **Decided: repeated settings.** libmc's YAML parser has anchors and aliases
  but no merge keys, so `<<: *rails_app` reads as an unknown setting. A
  top-level `include_defaults:` takes every include setting but `dir:` and
  `port:`; an app's setting replaces the default, and its `env:` adds to it.
- **`needs:` without `setup:`.** It then goes on every proc in the app, so all of
  them react when `db` restarts, not only a setup job.

### Code

As built: `src/config.c` reads each app as a config of its own, then merges
its procs into `config->procs` with the prefix (`read_includes`, `add_app`).
The daemon and supervisor need no change, since the merged specs look like
normal ones.
- Each app's `needs` indexes move by where its procs start in `config->procs`.
- `check_cycles` and `check_ports` run on each file before the merge, so
  they keep finding a proc's node by its index. No loop can cross files: an
  include's `needs:` names top-level procs only, and a top-level proc cannot
  need an app's. Ports across apps are checked while merging, against the
  include entry.
- `config_select` turns a proc or app name into indexes, for `resolve` in
  `src/server.c` and `bprocs run NAME`. `logs`, `attach`, input and `clear`
  take one proc and still need its full name.
- `Config.files` lists every file read, the `.env` and `env_file:` files
  included; the daemon sends it in its hello, and the TUI watches all of it.

## Phase 3: TUI grouping

Status: done. Beyond the plan below: a selected header's detail box adds up
its procs' usage and names the one most in need of attention, and its log
shows their lines together since the TUI opened, each after its short name.
`c` clears every log of the group, `o` opens its first port, and attach says
to pick a proc. Groups start collapsed and keep being expanded, and the
selection kept, by name across a reload. The worst state ranks a failure,
then a failing health check, a restart coming, starting or stopping,
running, stopped, idle, and last a finished job, so a done `setup` never
colours its header. The grouping is in `src/group.c`.

- Group procs by the part of the name before `:` with a header row. Top-level
  procs, with no `:`, stay ungrouped above the groups. The header shows the
  worst state of its children, so a crashed `web` is red on a collapsed group.
- Under a header, a proc shows by its short name (`web`), so long app names do
  not crowd the list.
- Collapse and expand a group with `h` and `l`, or Enter on a header row. Enter
  on a proc row still attaches. Start, stop and restart on a header act on the
  whole group.
- `1`…`9` and the mouse select the visible rows, headers included.
- `src/layout.c`, `src/ui.c`, `src/app.c`. Tests in `tests/test_screen.c`.

## Not doing

- **Mirroring a nested daemon's state into the outer one.** It needs two
  sockets per tab and goes stale. `include:` makes it unnecessary.
- **Treating `PORT` as the proc's `port` for a Procfile.** The README says
  bprocs cannot tell whether the program listens on it. Open question: show it in
  the list as an expected port, but do not use it for checks.

## Order and size

Phase 1 is done in bprocs; what is left of it is the switch to `bprocs run` in
`make-app/bprocs.yml`. Phase 2 is done in bprocs too, so that switch can go
straight to `include:`. Phase 3 follows soon
after: with a dozen apps in one list there are 30 to 50 rows, too many to use
without groups. After Phase 2, `make-app/bprocs.yml` becomes one `include:` per
app, and `bin/start.sh` stays `bprocs`.
