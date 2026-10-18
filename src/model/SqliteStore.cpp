// SQLite backend for the task forest (declared in model/Store.hpp).
//
// Schema (one row per task; `roots` / `doneRoots` / `children` are NOT stored as lists):
//
//   tasks(id, parent, ord, text, collapsed, status, created_at, done_at, deleted_at,
//         workspace_at, description)
//   notes(id, title, body, view, x, y, w, open, created_at, deleted_at)
//   meta(key, value)          -- next_id, next_note_id, current_view, desc_collapsed
//
// TWO TIMESTAMPS, NO BOOLEANS. `done_at` and `deleted_at` each carry both the state and
// its date, so the pair can never disagree the way a flag beside a timestamp can:
//
//   done_at = 0    not done          deleted_at = 0    live
//   done_at > 0    done, at that ms  deleted_at > 0    deleted, at that ms
//   done_at = -1   done, date unknown (completed before the date was recorded)
//
// NOTHING IS EVER HARD-DELETED. There is no DELETE statement in this file: removing a
// task stamps `deleted_at` and the row stays forever. Loads filter `deleted_at = 0`, so
// the Forest only ever holds live tasks — the trash is deliberately outside the model,
// readable via deletedRows(). An id that comes back (an undone delete) has `deleted_at`
// cleared to 0 by the upsert.
//
// `ord` is a task's position among its siblings, or among the top-level tasks (parent = 0).
// A done task keeps its parent and its `ord` — completion is a timestamp, not a move — so:
//
//   top-level tasks   parent = 0, ordered by ord     (done ones included)
//   children of P     parent = P, ordered by ord     (ditto)
//   canvas            tasks with no done_at anywhere on their ancestor chain
//   DONE section      done_at != 0, and its top entries are those whose parent chain has
//                     no other done task — so a done child of a live parent heads its own
//                     entry while staying in place under that parent
//
// Un-doing therefore needs no stored memory of where a task used to be: it never left.
//
// This is the whole-forest path (load once at startup, save the lot in one transaction),
// which is parity with the JSON store, not yet the incremental/concurrent story that
// motivated SQLite — see docs/FUTURE.md.

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <sqlite3.h>

#include "app/Paths.hpp"
#include "model/Store.hpp"

namespace tt::store {
namespace {

namespace fs = std::filesystem;

// 1 = original, 2 = + deleted_at, 3 = `done` boolean folded into done_at, 4 = + trees,
// 5 = trees replaced by workspace_at (a workspace is a marked task, not a partition),
// 6 = + description, 7 = + notes
constexpr int kSchemaVersion = 7;

const char* const kSchema =
    "CREATE TABLE IF NOT EXISTS tasks("
    "  id         INTEGER PRIMARY KEY,"
    "  parent     INTEGER NOT NULL DEFAULT 0,"
    "  ord        INTEGER NOT NULL DEFAULT 0,"
    "  text       TEXT    NOT NULL DEFAULT '',"
    "  collapsed  INTEGER NOT NULL DEFAULT 0,"
    "  status     INTEGER NOT NULL DEFAULT 0,"
    "  created_at INTEGER NOT NULL DEFAULT 0,"
    "  done_at    INTEGER NOT NULL DEFAULT 0,"    // 0 = not done; -1 = done, date unknown
    "  deleted_at INTEGER NOT NULL DEFAULT 0,"    // 0 = live; epoch ms = when it was deleted
    "  workspace_at INTEGER NOT NULL DEFAULT 0,"  // 0 = plain task; epoch ms = made a workspace
    "  description TEXT   NOT NULL DEFAULT '');"   // free multi-line text; '' = none
    "CREATE INDEX IF NOT EXISTS idx_tasks_parent ON tasks(parent, ord);"
    "CREATE TABLE IF NOT EXISTS notes("
    "  id         INTEGER PRIMARY KEY,"
    "  title      TEXT    NOT NULL DEFAULT '',"
    "  body       TEXT    NOT NULL DEFAULT '',"
    "  view       INTEGER NOT NULL DEFAULT 0,"    // 0 = main; else the workspace task's id
    "  x          REAL    NOT NULL DEFAULT 0,"    // top-left, world coordinates
    "  y          REAL    NOT NULL DEFAULT 0,"
    "  w          REAL    NOT NULL DEFAULT 0,"    // 0 = the default width
    "  open       INTEGER NOT NULL DEFAULT 1,"    // 0 = closed (kept, not drawn)
    "  created_at INTEGER NOT NULL DEFAULT 0,"
    "  deleted_at INTEGER NOT NULL DEFAULT 0);"   // soft delete, as for tasks
    "CREATE TABLE IF NOT EXISTS meta(key TEXT PRIMARY KEY, value NOT NULL);";

std::int64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// Owning handles: every early return below closes/finalizes without a goto ladder.
struct Db {
    sqlite3* h = nullptr;
    ~Db() { close(); }
    // sqlite3_open_v2 hands back a handle even when it FAILS, so failure paths (and the
    // Session, which retries opens) must be able to drop it explicitly.
    void close() {
        if (h) sqlite3_close(h);
        h = nullptr;
    }
    bool exec(const char* sql) const {
        return sqlite3_exec(h, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    }
};

struct Stmt {
    sqlite3_stmt* h = nullptr;
    ~Stmt() { if (h) sqlite3_finalize(h); }
    bool prepare(sqlite3* db, const char* sql) {
        return sqlite3_prepare_v2(db, sql, -1, &h, nullptr) == SQLITE_OK;
    }
};

int userVersion(const Db& db) {
    Stmt q;
    if (!q.prepare(db.h, "PRAGMA user_version") || sqlite3_step(q.h) != SQLITE_ROW) return -1;
    return sqlite3_column_int(q.h, 0);
}

// Schema 4 partitioned tasks into `trees`; 5 makes a workspace a marked task inside the one
// forest instead. Each tree other than main (id 1) becomes a workspace task: its sole
// top-level task if it has exactly one, else a new task named after the tree with its
// top-level tasks moved under it. The column to mark them does not exist yet at this point,
// so the chosen ids are parked in a temp table for applyPendingWorkspaces().
bool foldTreesIntoWorkspaces(const Db& db) {
    if (!db.exec("CREATE TEMP TABLE IF NOT EXISTS pending_ws(id INTEGER PRIMARY KEY)"))
        return false;
    std::vector<std::pair<std::int64_t, std::string>> trees;
    {
        Stmt q;
        if (!q.prepare(db.h, "SELECT id,name FROM trees WHERE id!=1 AND deleted_at=0 ORDER BY ord, id"))
            return false;
        while (sqlite3_step(q.h) == SQLITE_ROW) {
            const unsigned char* n = sqlite3_column_text(q.h, 1);
            trees.emplace_back(sqlite3_column_int64(q.h, 0), n ? reinterpret_cast<const char*>(n) : "");
        }
    }
    const std::int64_t now = nowMs();
    for (const auto& [tree, name] : trees) {
        std::vector<std::int64_t> roots;
        {
            Stmt q;
            if (!q.prepare(db.h, "SELECT id FROM tasks WHERE parent=0 AND tree=? AND deleted_at=0"
                                 " ORDER BY ord, id"))
                return false;
            sqlite3_bind_int64(q.h, 1, tree);
            while (sqlite3_step(q.h) == SQLITE_ROW) roots.push_back(sqlite3_column_int64(q.h, 0));
        }
        if (roots.empty()) continue;   // an empty tree has nothing to keep
        std::int64_t ws = roots.front();
        if (roots.size() > 1) {
            // A fresh id past both next_id and every row (deleted ones included).
            std::int64_t id = 1;
            Stmt q;
            if (!q.prepare(db.h, "SELECT MAX(COALESCE((SELECT CAST(value AS INTEGER) FROM meta"
                                 " WHERE key='next_id'),1),(SELECT COALESCE(MAX(id),0)+1 FROM tasks))") ||
                sqlite3_step(q.h) != SQLITE_ROW)
                return false;
            id = sqlite3_column_int64(q.h, 0);
            Stmt ins;
            if (!ins.prepare(db.h, "INSERT INTO tasks(id,parent,ord,text,created_at,tree) VALUES"
                                   "(?,0,(SELECT COALESCE(MAX(ord),0)+1 FROM tasks WHERE parent=0),?,?,1)"))
                return false;
            sqlite3_bind_int64(ins.h, 1, id);
            sqlite3_bind_text(ins.h, 2, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ins.h, 3, now);
            if (sqlite3_step(ins.h) != SQLITE_DONE) return false;
            for (std::size_t i = 0; i < roots.size(); ++i) {
                Stmt mv;
                if (!mv.prepare(db.h, "UPDATE tasks SET parent=?, ord=? WHERE id=?")) return false;
                sqlite3_bind_int64(mv.h, 1, id);
                sqlite3_bind_int(mv.h, 2, static_cast<int>(i));
                sqlite3_bind_int64(mv.h, 3, roots[i]);
                if (sqlite3_step(mv.h) != SQLITE_DONE) return false;
            }
            Stmt m;
            if (!m.prepare(db.h, "INSERT INTO meta(key,value) VALUES('next_id',?)"
                                 " ON CONFLICT(key) DO UPDATE SET value=excluded.value"))
                return false;
            sqlite3_bind_int64(m.h, 1, id + 1);
            if (sqlite3_step(m.h) != SQLITE_DONE) return false;
            ws = id;
        }
        Stmt p;
        if (!p.prepare(db.h, "INSERT OR IGNORE INTO pending_ws(id) VALUES(?)")) return false;
        sqlite3_bind_int64(p.h, 1, ws);
        if (sqlite3_step(p.h) != SQLITE_DONE) return false;
    }
    return true;
}

bool applyPendingWorkspaces(const Db& db) {
    Stmt q;
    if (!q.prepare(db.h, "UPDATE tasks SET workspace_at=? WHERE id IN (SELECT id FROM pending_ws)"))
        return false;
    sqlite3_bind_int64(q.h, 1, nowMs());
    if (sqlite3_step(q.h) != SQLITE_DONE) return false;
    return db.exec("DROP TABLE pending_ws");
}

// Bring an older file up to kSchemaVersion, one step at a time. No row is ever dropped,
// and no state is lost: a step that removes a column must first move its information
// somewhere else.
bool upgradeSteps(const Db& db, int from);

// All steps in one transaction: a step that fails part-way (the v4 fold writes several rows
// before it drops a column) must leave the file exactly as it was, still readable as `from`.
bool upgradeSchema(const Db& db) {
    const int from = userVersion(db);
    if (from < 0) return false;
    if (from == kSchemaVersion) return true;
    if (from > kSchemaVersion) return false;   // written by a newer build: refuse, don't corrupt
    if (!db.exec("BEGIN IMMEDIATE")) return false;
    if (!upgradeSteps(db, from)) {
        db.exec("ROLLBACK");
        return false;
    }
    return db.exec("COMMIT");
}

bool upgradeSteps(const Db& db, int from) {
    // 0 means this process just created the file, so kSchema already made it current.
    if (from >= 1) {
        if (from < 2 &&
            !db.exec("ALTER TABLE tasks ADD COLUMN deleted_at INTEGER NOT NULL DEFAULT 0"))
            return false;
        if (from < 3) {
            // Folding `done` into done_at. Tasks completed before the date was recorded are
            // done with done_at = 0, so they MUST be stamped kDoneAtUnknown first — dropping
            // the column without this would silently un-complete every one of them.
            if (!db.exec("UPDATE tasks SET done_at=-1 WHERE done!=0 AND done_at=0")) return false;
            // Symmetrically, a done_at on a not-done row was always meaningless; clear it so
            // done_at != 0 is exactly "done".
            if (!db.exec("UPDATE tasks SET done_at=0 WHERE done=0 AND done_at!=0")) return false;
            if (!db.exec("ALTER TABLE tasks DROP COLUMN done")) return false;
        }
        if (from == 4 && !foldTreesIntoWorkspaces(db)) return false;
        if (from < 5 &&
            !db.exec("ALTER TABLE tasks ADD COLUMN workspace_at INTEGER NOT NULL DEFAULT 0"))
            return false;
        if (from == 4) {
            // Now that every tree is a workspace task, the partition goes.
            if (!applyPendingWorkspaces(db) ||
                !db.exec("ALTER TABLE tasks DROP COLUMN tree") ||
                !db.exec("DROP TABLE IF EXISTS trees") ||
                !db.exec("DELETE FROM meta WHERE key='current_tree'"))
                return false;
        }
        if (from < 6 &&
            !db.exec("ALTER TABLE tasks ADD COLUMN description TEXT NOT NULL DEFAULT ''"))
            return false;
        // 7 adds the notes table, which kSchema's CREATE TABLE IF NOT EXISTS has already
        // made by the time this runs: nothing to move.
    }
    return db.exec(("PRAGMA user_version=" + std::to_string(kSchemaVersion)).c_str());
}

bool openDb(Db& db, const std::string& path, bool create) {
    const int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    if (sqlite3_open_v2(path.c_str(), &db.h, flags, nullptr) != SQLITE_OK) return false;
    // A second process (a CLI, an agent) may hold the write lock briefly; wait rather
    // than fail. WAL is what lets that reader work while the app has the DB open.
    sqlite3_busy_timeout(db.h, 3000);

    // Bail out on a store from a NEWER build before writing a single byte to it. Setting the
    // pragmas or running CREATE ... IF NOT EXISTS first would persist a journal-mode change and
    // add our index to a file we have just decided we must not touch.
    const int found = userVersion(db);
    if (found < 0 || found > kSchemaVersion) return false;

    db.exec("PRAGMA journal_mode=WAL");
    db.exec("PRAGMA synchronous=NORMAL");
    if (!db.exec(kSchema)) return false;
    return upgradeSchema(db);
}

// Position of every node within its sibling list (or among the top-level tasks).
std::unordered_map<TaskId, int> siblingOrder(const Forest& f) {
    std::unordered_map<TaskId, int> ord;
    ord.reserve(f.nodes.size());
    for (std::size_t i = 0; i < f.roots.size(); ++i) ord[f.roots[i]] = static_cast<int>(i);
    for (const auto& [id, t] : f.nodes)
        for (std::size_t i = 0; i < t.children.size(); ++i)
            ord[t.children[i]] = static_cast<int>(i);
    return ord;
}

int ordOf(const std::unordered_map<TaskId, int>& ord, TaskId id) {
    const auto it = ord.find(id);
    return it == ord.end() ? 0 : it->second;
}

// Do these two tasks produce identical columns? `children` is deliberately excluded —
// it is stored as the children's own parent+ord, not on this row.
bool sameRow(const Task& a, const Task& b) {
    return a.parent == b.parent && a.workspaceAt == b.workspaceAt && a.text == b.text &&
           a.collapsed == b.collapsed && a.status == b.status &&
           a.createdAt == b.createdAt && a.doneAt == b.doneAt && a.description == b.description;
}

bool loadNotes(const Db& db, Forest& f) {
    f.notes.clear();
    f.nextNoteId = 1;
    Stmt q;
    if (!q.prepare(db.h, "SELECT id,title,body,view,x,y,w,open,created_at"
                         " FROM notes WHERE deleted_at=0 ORDER BY id"))
        return false;
    for (;;) {
        const int rc = sqlite3_step(q.h);
        if (rc == SQLITE_DONE) break;
        if (rc != SQLITE_ROW) return false;
        Note n;
        n.id = static_cast<NoteId>(sqlite3_column_int64(q.h, 0));
        if (n.id == 0) continue;
        if (const unsigned char* s = sqlite3_column_text(q.h, 1))
            n.title = reinterpret_cast<const char*>(s);
        if (const unsigned char* s = sqlite3_column_text(q.h, 2))
            n.body = reinterpret_cast<const char*>(s);
        n.view = static_cast<TaskId>(sqlite3_column_int64(q.h, 3));
        n.x = static_cast<float>(sqlite3_column_double(q.h, 4));
        n.y = static_cast<float>(sqlite3_column_double(q.h, 5));
        n.w = static_cast<float>(sqlite3_column_double(q.h, 6));
        n.open = sqlite3_column_int(q.h, 7) != 0;
        n.createdAt = sqlite3_column_int64(q.h, 8);
        f.nextNoteId = std::max<NoteId>(f.nextNoteId, n.id + 1);
        f.notes.emplace(n.id, std::move(n));
    }
    Stmt m;
    if (m.prepare(db.h, "SELECT value FROM meta WHERE key='next_note_id'") &&
        sqlite3_step(m.h) == SQLITE_ROW)
        f.nextNoteId = std::max<NoteId>(f.nextNoteId,
                                        static_cast<NoteId>(sqlite3_column_int64(m.h, 0)));
    return true;
}

// Notes take the same incremental path as tasks: with a baseline, only changed notes are
// written and a deletion is "in baseline, gone from f"; without one, every note is written
// and any other live row is retired. Runs inside saveToDb's transaction.
bool saveNotes(const Db& db, const Forest& f, const Forest* baseline) {
    std::vector<NoteId> upserts, deletes;
    if (baseline) {
        for (const auto& [id, n] : f.notes) {
            const Note* b = baseline->note(id);
            if (!b || !sameNote(n, *b)) upserts.push_back(id);
        }
        for (const auto& [id, n] : baseline->notes)
            if (!f.note(id)) deletes.push_back(id);
    } else {
        for (const auto& [id, n] : f.notes) upserts.push_back(id);
        Stmt q;
        if (!q.prepare(db.h, "SELECT id FROM notes WHERE deleted_at=0")) return false;
        while (sqlite3_step(q.h) == SQLITE_ROW) {
            const NoteId id = static_cast<NoteId>(sqlite3_column_int64(q.h, 0));
            if (!f.note(id)) deletes.push_back(id);
        }
    }

    if (!upserts.empty()) {
        Stmt up;
        if (!up.prepare(db.h,
                        "INSERT INTO notes(id,title,body,view,x,y,w,open,created_at,deleted_at)"
                        " VALUES(?,?,?,?,?,?,?,?,?,0)"
                        " ON CONFLICT(id) DO UPDATE SET title=excluded.title,"
                        " body=excluded.body, view=excluded.view, x=excluded.x, y=excluded.y,"
                        " w=excluded.w, open=excluded.open, created_at=excluded.created_at,"
                        " deleted_at=0"))
            return false;
        for (NoteId id : upserts) {
            const Note* n = f.note(id);
            sqlite3_reset(up.h);
            sqlite3_bind_int64(up.h, 1, static_cast<sqlite3_int64>(n->id));
            sqlite3_bind_text(up.h, 2, n->title.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(up.h, 3, n->body.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int64(up.h, 4, static_cast<sqlite3_int64>(n->view));
            sqlite3_bind_double(up.h, 5, n->x);
            sqlite3_bind_double(up.h, 6, n->y);
            sqlite3_bind_double(up.h, 7, n->w);
            sqlite3_bind_int(up.h, 8, n->open ? 1 : 0);
            sqlite3_bind_int64(up.h, 9, n->createdAt);
            if (sqlite3_step(up.h) != SQLITE_DONE) return false;
        }
    }

    if (!deletes.empty()) {
        Stmt del;
        if (!del.prepare(db.h, "UPDATE notes SET deleted_at=? WHERE id=? AND deleted_at=0"))
            return false;
        const std::int64_t when = nowMs();
        for (NoteId id : deletes) {
            sqlite3_reset(del.h);
            sqlite3_bind_int64(del.h, 1, when);
            sqlite3_bind_int64(del.h, 2, static_cast<sqlite3_int64>(id));
            if (sqlite3_step(del.h) != SQLITE_DONE) return false;
        }
    }

    // Like next_id, never backwards: another writer may already have used higher ids.
    Stmt m;
    if (!m.prepare(db.h, "INSERT INTO meta(key,value) VALUES('next_note_id',?)"
                         " ON CONFLICT(key) DO UPDATE SET"
                         " value=MAX(CAST(value AS INTEGER),CAST(excluded.value AS INTEGER))"))
        return false;
    sqlite3_bind_int64(m.h, 1, static_cast<sqlite3_int64>(f.nextNoteId));
    return sqlite3_step(m.h) == SQLITE_DONE;
}

// The read itself, given an open connection. Shared by loadDb (fresh connection per
// call) and Session::load (one held connection).
bool loadFromDb(const Db& db, Forest& f) {
    f.nodes.clear();
    f.roots.clear();
    f.nextId = 1;

    // Grouped by parent and in sibling order, so a single appending pass rebuilds
    // `children` and `roots` in exactly the order they were written.
    std::vector<TaskId> rowOrder;
    {
        Stmt q;
        if (!q.prepare(db.h,
                       "SELECT id,parent,text,collapsed,status,created_at,done_at,workspace_at,description"
                       " FROM tasks WHERE deleted_at=0 ORDER BY parent, ord, id"))
            return false;
        for (;;) {
            const int rc = sqlite3_step(q.h);
            if (rc == SQLITE_DONE) break;
            if (rc != SQLITE_ROW) return false;

            Task t;
            t.id = static_cast<TaskId>(sqlite3_column_int64(q.h, 0));
            if (t.id == 0) continue;      // 0 means "no parent"; never a real id
            t.parent = static_cast<TaskId>(sqlite3_column_int64(q.h, 1));
            if (const unsigned char* s = sqlite3_column_text(q.h, 2))
                t.text = reinterpret_cast<const char*>(s);
            t.collapsed = sqlite3_column_int(q.h, 3) != 0;
            t.status = sqlite3_column_int(q.h, 4);
            t.createdAt = sqlite3_column_int64(q.h, 5);
            t.doneAt = sqlite3_column_int64(q.h, 6);
            t.workspaceAt = sqlite3_column_int64(q.h, 7);
            if (const unsigned char* s = sqlite3_column_text(q.h, 8))
                t.description = reinterpret_cast<const char*>(s);

            rowOrder.push_back(t.id);
            f.nextId = std::max<TaskId>(f.nextId, t.id + 1);
            f.nodes.emplace(t.id, std::move(t));
        }
    }

    for (TaskId id : rowOrder) {
        Task* t = f.get(id);
        if (!t) continue;
        // Defensive, matching the JSON loader: a dangling parent becomes a root rather
        // than an unreachable node.
        if (t->parent != kNoParent && !f.exists(t->parent)) t->parent = kNoParent;
        if (t->parent == kNoParent) f.roots.push_back(id);   // done or not: same list
        else                        f.get(t->parent)->children.push_back(id);
    }

    Stmt m;
    if (m.prepare(db.h, "SELECT value FROM meta WHERE key='next_id'") &&
        sqlite3_step(m.h) == SQLITE_ROW)
        f.nextId = std::max<TaskId>(f.nextId, static_cast<TaskId>(sqlite3_column_int64(m.h, 0)));
    return loadNotes(db, f);
}

// The write itself, given an open connection. Shared by saveDb and Session::save.
bool saveToDb(const Db& db, const Forest& f, const Forest* baseline) {
    // One transaction per save: a reader sees the previous state or the new one, never a
    // half-written tree. IMMEDIATE takes the write lock up front.
    if (!db.exec("BEGIN IMMEDIATE")) return false;

    bool ok = true;
    const std::unordered_map<TaskId, int> ord = siblingOrder(f);

    // Work out the minimum set of statements. `deletes` are SOFT: see the file header.
    std::vector<TaskId> upserts;
    std::vector<TaskId> deletes;
    if (baseline) {
        // Incremental. `ord` is compared too: removing a middle child renumbers its later
        // siblings without changing a single field on their rows.
        const std::unordered_map<TaskId, int> was = siblingOrder(*baseline);
        for (const auto& [id, t] : f.nodes) {
            const Task* b = baseline->get(id);
            if (!b || !sameRow(t, *b) || ordOf(ord, id) != ordOf(was, id)) upserts.push_back(id);
        }
        for (const auto& [id, t] : baseline->nodes)
            if (!f.exists(id)) deletes.push_back(id);   // WE deleted it; absence alone is not proof
    } else {
        // Full rewrite: every node, and retire whatever else is still live.
        for (const auto& [id, t] : f.nodes) upserts.push_back(id);
        Stmt q;
        if (q.prepare(db.h, "SELECT id FROM tasks WHERE deleted_at=0")) {
            while (sqlite3_step(q.h) == SQLITE_ROW) {
                const TaskId id = static_cast<TaskId>(sqlite3_column_int64(q.h, 0));
                if (!f.exists(id)) deletes.push_back(id);
            }
        } else {
            ok = false;
        }
    }

    if (ok && !upserts.empty()) {
        Stmt up;
        // deleted_at=0 on both paths: a task present in the forest is live by definition,
        // so writing one un-deletes its row (this is what makes undo-of-a-delete work).
        ok = up.prepare(db.h,
                        "INSERT INTO tasks(id,parent,ord,text,collapsed,status,"
                        "created_at,done_at,deleted_at,workspace_at,description)"
                        " VALUES(?,?,?,?,?,?,?,?,0,?,?)"
                        " ON CONFLICT(id) DO UPDATE SET parent=excluded.parent,"
                        " ord=excluded.ord, text=excluded.text,"
                        " collapsed=excluded.collapsed, status=excluded.status,"
                        " created_at=excluded.created_at, done_at=excluded.done_at,"
                        " deleted_at=0, workspace_at=excluded.workspace_at,"
                        " description=excluded.description");
        for (TaskId id : upserts) {
            if (!ok) break;
            const Task* t = f.get(id);
            if (!t) continue;
            sqlite3_reset(up.h);
            sqlite3_bind_int64(up.h, 1, static_cast<sqlite3_int64>(t->id));
            sqlite3_bind_int64(up.h, 2, static_cast<sqlite3_int64>(t->parent));
            sqlite3_bind_int(up.h, 3, ordOf(ord, id));
            // SQLITE_STATIC: `*t` lives in the forest, which outlives this step().
            sqlite3_bind_text(up.h, 4, t->text.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_int(up.h, 5, t->collapsed ? 1 : 0);
            sqlite3_bind_int(up.h, 6, t->status);
            sqlite3_bind_int64(up.h, 7, t->createdAt);
            sqlite3_bind_int64(up.h, 8, t->doneAt);
            sqlite3_bind_int64(up.h, 9, t->workspaceAt);
            sqlite3_bind_text(up.h, 10, t->description.c_str(), -1, SQLITE_STATIC);
            ok = sqlite3_step(up.h) == SQLITE_DONE;
        }
    }

    if (ok && !deletes.empty()) {
        // Soft delete: stamp the row and keep it. `AND deleted_at=0` preserves the FIRST
        // deletion's timestamp if this ever runs twice for the same id.
        Stmt del;
        ok = del.prepare(db.h, "UPDATE tasks SET deleted_at=? WHERE id=? AND deleted_at=0");
        const std::int64_t when = nowMs();
        for (TaskId id : deletes) {
            if (!ok) break;
            sqlite3_reset(del.h);
            sqlite3_bind_int64(del.h, 1, when);
            sqlite3_bind_int64(del.h, 2, static_cast<sqlite3_int64>(id));
            ok = sqlite3_step(del.h) == SQLITE_DONE;
        }
    }

    if (ok) {
        // next_id never moves backwards: another writer may have taken ids beyond ours,
        // and lowering it would hand out an id that is already in use.
        Stmt m;
        ok = m.prepare(db.h, "INSERT INTO meta(key,value) VALUES('next_id',?)"
                             " ON CONFLICT(key) DO UPDATE SET"
                             " value=MAX(CAST(value AS INTEGER),CAST(excluded.value AS INTEGER))");
        if (ok) {
            sqlite3_bind_int64(m.h, 1, static_cast<sqlite3_int64>(f.nextId));
            ok = sqlite3_step(m.h) == SQLITE_DONE;
        }
    }

    if (ok) ok = saveNotes(db, f, baseline);

    if (!ok) {
        db.exec("ROLLBACK");
        return false;
    }
    return db.exec("COMMIT");
}

// The counter SQLite bumps for every commit made by a connection OTHER than the one
// asking — the entire basis of Session::changedExternally(). -1 on failure.
std::int64_t dataVersionOf(const Db& db) {
    Stmt q;
    if (!q.prepare(db.h, "PRAGMA data_version") || sqlite3_step(q.h) != SQLITE_ROW) return -1;
    return sqlite3_column_int64(q.h, 0);
}

} // namespace

bool loadDb(Forest& f, const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return false;   // absent -> empty forest, as with JSON
    Db db;
    if (!openDb(db, path, false)) return false;
    return loadFromDb(db, f);
}

bool saveDb(const Forest& f, const std::string& path, const Forest* baseline) {
    const fs::path p(path);
    if (auto dir = p.parent_path(); !dir.empty()) paths::ensureDir(dir);
    Db db;
    if (!openDb(db, path, true)) return false;
    return saveToDb(db, f, baseline);
}

struct Session::Impl {
    Db db;
    bool open = false;
    std::int64_t dataVersion = 0;

    // Open (or keep) the held connection, and take the data_version baseline the moment
    // it opens — our own later commits will not move it, other writers' will.
    bool ensureOpen(const std::string& path, bool create) {
        if (open) return true;
        if (!openDb(db, path, create)) {
            db.close();   // sqlite3_open_v2 hands back a handle even on failure
            return false;
        }
        dataVersion = dataVersionOf(db);
        open = true;
        return true;
    }
    void drop() {
        db.close();
        open = false;
    }
};

Session::Session(std::string path) : path_(std::move(path)), impl_(std::make_unique<Impl>()) {}
Session::~Session() = default;

bool Session::load(Forest& f) {
    if (!isDbPath(path_)) return loadJson(f, path_);
    // No CREATE here: loading must not invent a store (matches loadDb's absent -> false).
    if (!impl_->ensureOpen(path_, false)) return false;
    if (!loadFromDb(impl_->db, f)) {
        // A connection that failed to read holds no promise it can be trusted to write —
        // and dropping it frees the file for quarantine().
        impl_->drop();
        return false;
    }
    impl_->dataVersion = dataVersionOf(impl_->db);
    return true;
}

bool Session::save(const Forest& f, const Forest* baseline) {
    if (!isDbPath(path_)) return saveJson(f, path_);
    const fs::path p(path_);
    if (auto dir = p.parent_path(); !dir.empty()) paths::ensureDir(dir);
    if (!impl_->ensureOpen(path_, true)) return false;
    return saveToDb(impl_->db, f, baseline);
}

TaskId Session::currentView() {
    if (!isDbPath(path_) || !impl_->ensureOpen(path_, false)) return kMainView;
    Stmt q;
    if (!q.prepare(impl_->db.h, "SELECT value FROM meta WHERE key='current_view'") ||
        sqlite3_step(q.h) != SQLITE_ROW)
        return kMainView;
    return static_cast<TaskId>(sqlite3_column_int64(q.h, 0));
}

bool Session::setCurrentView(TaskId view) {
    if (!isDbPath(path_) || !impl_->ensureOpen(path_, true)) return false;
    Stmt q;
    if (!q.prepare(impl_->db.h, "INSERT INTO meta(key,value) VALUES('current_view',?)"
                                " ON CONFLICT(key) DO UPDATE SET value=excluded.value"))
        return false;
    sqlite3_bind_int64(q.h, 1, static_cast<sqlite3_int64>(view));
    return sqlite3_step(q.h) == SQLITE_DONE;
}

bool Session::descCollapsed() {
    if (!isDbPath(path_) || !impl_->ensureOpen(path_, false)) return false;
    Stmt q;
    if (!q.prepare(impl_->db.h, "SELECT value FROM meta WHERE key='desc_collapsed'") ||
        sqlite3_step(q.h) != SQLITE_ROW)
        return false;
    return sqlite3_column_int(q.h, 0) != 0;
}

bool Session::setDescCollapsed(bool collapsed) {
    if (!isDbPath(path_) || !impl_->ensureOpen(path_, true)) return false;
    Stmt q;
    if (!q.prepare(impl_->db.h, "INSERT INTO meta(key,value) VALUES('desc_collapsed',?)"
                                " ON CONFLICT(key) DO UPDATE SET value=excluded.value"))
        return false;
    sqlite3_bind_int(q.h, 1, collapsed ? 1 : 0);
    return sqlite3_step(q.h) == SQLITE_DONE;
}

bool Session::changedExternally() {
    if (!isDbPath(path_)) return false;
    if (!impl_->open) {
        // No connection means our last look found nothing (or nothing readable). If the
        // store can be opened NOW, someone made one behind our back: that is a change,
        // and its content is by definition unseen. No CREATE — polling must not invent
        // the file it is watching for.
        return impl_->ensureOpen(path_, false);
    }
    const std::int64_t v = dataVersionOf(impl_->db);
    if (v < 0 || v == impl_->dataVersion) return false;
    impl_->dataVersion = v;
    return true;
}

int supportedDbSchemaVersion() { return kSchemaVersion; }

int dbSchemaVersion(const std::string& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) return -1;
    Db db;
    // Deliberately NOT openDb: that applies the schema and the upgrade ladder, and this is
    // asked precisely when we may have no business writing to the file at all.
    if (sqlite3_open_v2(path.c_str(), &db.h, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        return -1;
    return userVersion(db);
}

std::vector<DeletedRow> deletedRows(const std::string& path) {
    std::vector<DeletedRow> out;
    std::error_code ec;
    if (!fs::exists(path, ec)) return out;
    Db db;
    if (!openDb(db, path, false)) return out;

    Stmt q;
    if (!q.prepare(db.h, "SELECT id,parent,text,deleted_at FROM tasks WHERE deleted_at!=0"
                         " ORDER BY deleted_at DESC, id"))
        return out;
    while (sqlite3_step(q.h) == SQLITE_ROW) {
        DeletedRow r;
        r.id = static_cast<TaskId>(sqlite3_column_int64(q.h, 0));
        r.parent = static_cast<TaskId>(sqlite3_column_int64(q.h, 1));
        if (const unsigned char* s = sqlite3_column_text(q.h, 2))
            r.text = reinterpret_cast<const char*>(s);
        r.deletedAt = sqlite3_column_int64(q.h, 3);
        out.push_back(std::move(r));
    }
    return out;
}

} // namespace tt::store
