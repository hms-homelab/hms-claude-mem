# SDD-004: Recipes, ready-to-run scripts shared by every session

**Status:** Accepted 2026-11-20
**Author:** Albin + Claude
**Date:** 2026-11-20
**Target version:** 3.0.0 (new tools, existing tools unchanged)

## 1. Problem / Motivation

Memories work well for passing rules and decisions between sessions. They work
badly for scripts. Today a useful script (a SQL query against a service's
database, a shell helper that reaches a host, a Python snippet that mints a
token) ends up in one of three places:

1. **Inside a memory's free text**, mixed with prose. A session has to find the
   memory, then cut the script out of the paragraph, then guess its parameters.
2. **Inside a skill.** A skill is loaded only on the machine and in the project
   where it was written, so a session elsewhere never sees it.
3. **Nowhere.** It was written in a session, it worked, and the session ended.
   The next session writes it again from scratch, and sometimes gets it wrong.

A script also has a shape a memory does not: a language, a place it runs, the
parameters it takes, what it needs beforehand, whether it changes anything, and
when it was last seen working. Putting that in prose loses it.

And scripts in the main memory pool pollute rule searches: a search for "how
releases are tagged" should return the rule, not a 200-line deploy script.

## 2. Goal

A **recipe**: a named, structured, ready-to-run script, stored in claude-mem,
found by exact name or tag or by meaning, and reachable from every session on
every machine through the same server that serves memories.

## 3. Goals / Non-goals

**Goals**
- New tools: `recipe_create`, `recipe_update`, `recipe_delete`, `recipe_get`,
  `recipe_search`.
- Search two ways: exact (name, tag, language, runs_on) and semantic (meaning,
  same embedding and vector index kind memories use).
- Recipes live apart from memories: no `mem_*` tool sees a recipe, and no
  `recipe_*` tool sees a memory.
- One recipe pool shared by every client of the server.
- Both store providers (Redis and the embedded store) support recipes.

**Non-goals**
- **Running recipes.** The server stores and returns them. The session runs a
  recipe with its own tools and under its own permission rules, exactly as if it
  had written the script itself.
- A new database. No Postgres/pgvector: recipes use the store and vector index
  claude-mem already has.
- Version history of a recipe (see open decision D4).

## 4. The recipe

| Field | Required | Meaning |
|---|---|---|
| `name` | yes | Unique, exact, kebab-case: `ingest-nights-for-user`. The key. |
| `description` | yes | One or two sentences: what it does and when to use it. Embedded for search. |
| `language` | yes | `bash`, `zsh`, `sql`, `python`, `powershell`, `js`, ... |
| `script` | yes | The script itself, runnable as is once parameters are filled. |
| `runs_on` | yes | Where it runs: a list of host labels (`mac`, `hub`, `vps`, `windows`, `docker`, `any`). |
| `params` | no | List of `{name, description, example}`. Placeholders in the script are `{{name}}`. |
| `prereqs` | no | What must be true first: tools installed, a secrets file sourced (by variable NAME, never value), a tunnel up. |
| `mutates` | yes | `true` if it changes data or a live system (write SQL, deploy, delete, restart). Default `false` is never assumed: the caller must say. |
| `tags` | no | Free labels for exact filtering: `cpapdash`, `ingest`, `redis`, `github`. |
| `verified_at` | auto | Date the recipe was last confirmed working. Set by `recipe_update` with `verified: true`. |
| `created_at`, `updated_at` | auto | As for memories. |

Host labels are plain words, not addresses. The addresses live in memories or
in the session's own context, so this public design and the recipes carry no
private network detail by themselves.

### 4.1 Rules every recipe follows

- **No secrets.** A recipe names the secrets file and variable it reads
  (`source ~/.secrets; $SERVICE_TOKEN`); it never holds a value. See D3.
- **`mutates: true` changes nothing about permission.** A session still asks
  before running it, as for any action that changes data or a live system. The
  flag is there so the session knows to ask.
- **A broken recipe is fixed in place.** The session that finds a recipe no
  longer works fixes it with `recipe_update` once the fix is proven, rather than
  writing a private copy.

## 5. Tools

All return JSON like the `mem_*` tools.

- **`recipe_create(name, description, language, script, runs_on, mutates, params?, prereqs?, tags?)`**
  Refuses if `name` exists (use `recipe_update`). Refuses a `script` over 64 KB.
- **`recipe_update(name, ...any fields..., verified?)`**
  Changes only the fields given. `verified: true` stamps `verified_at` with
  today. Refuses if `name` does not exist. Re-embeds when `description`, `name`
  or `tags` change.
- **`recipe_delete(name)`**
- **`recipe_get(name)`** Exact name, the whole recipe.
- **`recipe_search(query?, name?, tag?, language?, runs_on?, mutates?, top_k=5)`**
  - With `name`: exact match, same as `recipe_get`.
  - With only filters (`tag`, `language`, `runs_on`, `mutates`): every recipe
    matching all of them, by name.
  - With `query`: semantic search, then the filters applied to the results.
  - Results show name, description, language, runs_on, mutates, verified_at;
    the script only with `recipe_get`, so a search stays small.

The `mem_*` tools are unchanged.

## 6. Storage

Recipes are a second, fixed pool beside the server's memory namespace:

- **Redis:** `claude:recipe:vectors` (vector set) and `claude:recipe:data:<name>`
  (hash, one field per recipe field; lists as JSON). Not under
  `claude:mem:<namespace>:`, so recipes are shared whatever `NAMESPACE` a server
  runs with.
- **Embedded store:** a separate file `recipes.db` beside the namespace file.

The embedded text is `<language>: <name>. <description> [tags]`. The script is
not embedded: its words would drown the description.

Exact filters scan the recipe hashes. Recipes are expected in the hundreds, not
the tens of thousands, so a scan is cheap and needs no secondary index.

Implementation: `IMemoryStore` gains no new methods. The server opens a second
store instance for the recipe pool (prefix `claude:recipe:`), and a new
`RecipeTools` sits beside `MemoryTools`, sharing the embedder.

## 7. Seeding

Once the tools ship, the scripts already scattered in memories, skills and
recent sessions are moved into recipes: each one checked to still work (a
read-only one by running it, a `mutates` one by reading it, never by running
it), stored with `verified_at`, and the memory that held it trimmed to point at
the recipe name. Which scripts is a list for Albin's OK before it starts (D5).

### 7.1 Claude Code's built-in memory

Claude Code has its own file memory (`~/.claude/projects/<project>/memory/*.md`
plus a `MEMORY.md` index loaded into every session). It needs no server, which
is its strength, but it does not fit recipes as their home:

- It is **per machine and per project directory**. A session on another host,
  or in another project folder on the same host, never sees it. Recipes are
  meant for every session everywhere.
- It has **no structured or semantic search**: a session finds a file through
  the index, and the index is read into every session's context, so hundreds
  of scripts listed there cost every session tokens.

It is the **main seed source**: on the author's main machine, 35 of 536 file
memories hold a script (SQL, bash, sh, js, ts). Seeding moves each one into a
recipe and leaves the file memory with its rule or fact and a one-line pointer
to the recipe name. File memories stay what they are good at: rules and
preferences for that project.

When the server cannot be reached, recipes cannot be reached either. See D6.

## 8. How sessions learn to use it

A pinned memory and the project CLAUDE.md line: "Before writing a script,
`recipe_search` for it. After writing one that worked and will be needed again,
`recipe_create` it."

## 9. Decisions

Taken by Albin on 2026-11-20 unless marked open.

- **D1. Tool names.** Separate tools, `recipe_create`, `recipe_update`,
  `recipe_delete`, `recipe_get`, `recipe_search`, like `mem_*` (Albin's own
  list: create, modify, delete, search by exact match and by vector).
- **D2. Who may delete.** Any session may delete a recipe, as it may create or
  update one.
- **D3. Secret guard.** The server refuses a `recipe_create` or
  `recipe_update` whose script, prereqs or params look like they hold a secret:
  well-known key prefixes (cloud, GitHub, Stripe, Slack, OpenAI and Anthropic
  style keys, PEM private key blocks) and a long opaque value assigned to a
  name containing `password`, `secret`, `token` or `key`. The refusal names the
  line, never echoes the value, and says to read it from a secrets file
  instead. A reference such as `$SERVICE_TOKEN` or `${API_KEY}` passes.
- **D4. History.** None. A bad update is fixed by another update;
  `verified_at` shows when the recipe last worked.
- **D5. Seed list.** Albin approves the list of scripts to move in before
  the seeding starts.
- **D6. Server down.** Accepted: recipes are unreachable while the server is
  down, as memories are. One place to fix a recipe; no second copy to go
  stale.

## 10. Testing

- Unit: create/update/delete/get; refuse duplicate create, refuse update of a
  missing name, refuse an oversized script; filters alone, query alone, both;
  re-embed on description change and not on script change; recipes invisible to
  `mem_list`/`mem_search` and memories invisible to `recipe_search`.
- Both providers: the same suite against Redis (throwaway prefix) and the
  embedded store.
- On the hub: the daemon after upgrade serves `mem_*` unchanged (the existing
  parity bench) and the new tools over HTTP.
