/* Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT. */
#include "client.hpp"

#include "json.hpp"
#include "rpc.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace haikuide {
namespace {

Value RangeOf(Pos start, Pos end) {
    Value range = JsonObject();
    Value s = JsonObject();
    Put(s, "line", JsonInt(start.line));
    Put(s, "character", JsonInt(start.character));
    Value e = JsonObject();
    Put(e, "line", JsonInt(end.line));
    Put(e, "character", JsonInt(end.character));
    Put(range, "start", std::move(s));
    Put(range, "end", std::move(e));
    return range;
}

bool ReadPos(const Value* range, const char* side, Pos* pos) {
    if (range == nullptr) {
        return false;
    }
    const Value* at = range->find(side);
    if (at == nullptr) {
        return false;
    }
    const Value* line = at->find("line");
    const Value* character = at->find("character");
    if (line == nullptr || character == nullptr || !line->is_int || !character->is_int) {
        return false;
    }
    pos->line = static_cast<int>(line->i);
    pos->character = static_cast<int>(character->i);
    return true;
}

std::string HoverText(const Value& contents) {
    if (contents.kind == Value::Kind::String) {
        return contents.s;
    }
    if (contents.kind == Value::Kind::Object) {
        if (const Value* value = contents.find("value")) {
            if (value->kind == Value::Kind::String) {
                return value->s;
            }
        }
    }
    if (contents.kind == Value::Kind::Array) {
        std::string out;
        for (const Value& part : contents.a) {
            const std::string piece = HoverText(part);
            if (piece.empty()) {
                continue;
            }
            if (!out.empty()) {
                out.push_back('\n');
            }
            out += piece;
        }
        return out;
    }
    return std::string();
}

void SetCloseOnExec(int fd) {
    const int flags = fcntl(fd, F_GETFD);
    if (flags >= 0) {
        fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
    }
}

void SetNonBlock(int fd) {
    const int flags = fcntl(fd, F_GETFL);
    if (flags >= 0) {
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
}

void CloseFd(int* fd) {
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

}  // namespace

struct LspClient::Process {
    std::string command;
    LspClient::Handler* handler = nullptr;
    std::mutex mu;
    std::string queued;
    std::string pending;
    std::atomic<bool> stop{false};
    std::atomic<bool> reaped{false};
    std::atomic<bool> ready{false};
    std::atomic<int> sync_kind{2};
    std::atomic<int64_t> next_id{1};
    std::atomic<int64_t> completion_id{0};
    std::atomic<int64_t> hover_id{0};
    std::atomic<int64_t> definition_id{0};
    int in_fd = -1;
    int out_fd = -1;
    int err_fd = -1;
    int wake_r = -1;
    int wake_w = -1;
    pid_t pid = -1;
    int status = 0;
    std::thread thread;
    std::string stderr_tail;
};

LspClient::LspClient() = default;

LspClient::~LspClient() { Stop(); }

bool LspClient::running() const { return proc_ != nullptr && !proc_->reaped.load(); }

void LspClient::Fail(const std::string& message) {
    Handler* handler = nullptr;
    if (proc_ != nullptr) {
        std::lock_guard<std::mutex> lock(proc_->mu);
        handler = proc_->stop.load() ? nullptr : proc_->handler;
    }
    if (handler != nullptr) {
        handler->OnServerExit(message);
    }
}

bool LspClient::Start(const std::string& command, const std::vector<std::string>& args,
                      const std::string& root_path, Handler* handler, std::string* err) {
    Stop();
    static std::once_flag once;
    std::call_once(once, [] { signal(SIGPIPE, SIG_IGN); });

    int child_in[2] = {-1, -1};
    int child_out[2] = {-1, -1};
    int child_err[2] = {-1, -1};
    int wake[2] = {-1, -1};
    if (pipe(child_in) != 0 || pipe(child_out) != 0 || pipe(child_err) != 0 || pipe(wake) != 0) {
        *err = "pipe failed";
        CloseFd(&child_in[0]);
        CloseFd(&child_in[1]);
        CloseFd(&child_out[0]);
        CloseFd(&child_out[1]);
        CloseFd(&child_err[0]);
        CloseFd(&child_err[1]);
        CloseFd(&wake[0]);
        CloseFd(&wake[1]);
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, child_in[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, child_out[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, child_err[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, child_in[1]);
    posix_spawn_file_actions_addclose(&actions, child_out[0]);
    posix_spawn_file_actions_addclose(&actions, child_err[0]);
    posix_spawn_file_actions_addclose(&actions, wake[0]);
    posix_spawn_file_actions_addclose(&actions, wake[1]);

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);

    std::vector<char*> argv;
    argv.push_back(const_cast<char*>(command.c_str()));
    std::vector<std::string> storage = args;
    for (std::string& arg : storage) {
        argv.push_back(arg.data());
    }
    argv.push_back(nullptr);

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, command.c_str(), &actions, &attr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    CloseFd(&child_in[0]);
    CloseFd(&child_out[1]);
    CloseFd(&child_err[1]);
    if (rc != 0) {
        CloseFd(&child_in[1]);
        CloseFd(&child_out[0]);
        CloseFd(&child_err[0]);
        CloseFd(&wake[0]);
        CloseFd(&wake[1]);
        *err = std::string("cannot start ") + command + ": " + std::strerror(rc);
        return false;
    }

    SetCloseOnExec(child_in[1]);
    SetCloseOnExec(child_out[0]);
    SetCloseOnExec(child_err[0]);
    SetCloseOnExec(wake[0]);
    SetCloseOnExec(wake[1]);
    SetNonBlock(child_in[1]);
    SetNonBlock(child_out[0]);
    SetNonBlock(child_err[0]);
    SetNonBlock(wake[0]);
    SetNonBlock(wake[1]);

    proc_ = new Process();
    proc_->command = command;
    proc_->handler = handler;
    proc_->in_fd = child_in[1];
    proc_->out_fd = child_out[0];
    proc_->err_fd = child_err[0];
    proc_->wake_r = wake[0];
    proc_->wake_w = wake[1];
    proc_->pid = pid;

    Value params = JsonObject();
    Put(params, "processId", JsonInt(static_cast<int64_t>(getpid())));
    Put(params, "rootUri", JsonString(PathToUri(root_path)));
    Value caps = JsonObject();
    Value text_doc = JsonObject();
    Value sync = JsonObject();
    Put(sync, "didSave", JsonBool(true));
    Put(sync, "dynamicRegistration", JsonBool(false));
    Put(text_doc, "synchronization", std::move(sync));
    Value completion = JsonObject();
    Value item = JsonObject();
    Put(item, "snippetSupport", JsonBool(false));
    Put(completion, "completionItem", std::move(item));
    Put(text_doc, "completion", std::move(completion));
    Value hover = JsonObject();
    Value formats = JsonArray();
    Push(formats, JsonString("plaintext"));
    Put(hover, "contentFormat", std::move(formats));
    Put(text_doc, "hover", std::move(hover));
    Put(caps, "textDocument", std::move(text_doc));
    Put(params, "capabilities", std::move(caps));
    Value init_opts = JsonObject();
    Value flags = JsonArray();
    Push(flags, JsonString("-std=c++17"));
    Put(init_opts, "fallbackFlags", std::move(flags));
    Put(params, "initializationOptions", std::move(init_opts));
    Value client = JsonObject();
    Put(client, "name", JsonString("HaikuIDE"));
    Put(client, "version", JsonString("0.3"));
    Put(params, "clientInfo", std::move(client));

    Value msg = JsonObject();
    Put(msg, "jsonrpc", JsonString("2.0"));
    Put(msg, "id", JsonInt(proc_->next_id.fetch_add(1)));
    Put(msg, "method", JsonString("initialize"));
    Put(msg, "params", std::move(params));
    {
        std::lock_guard<std::mutex> lock(proc_->mu);
        proc_->queued = EncodeFrame(Stringify(msg));
    }
    proc_->thread = std::thread([this] { Loop(); });
    return true;
}

void LspClient::Stop() {
    if (proc_ == nullptr) {
        return;
    }
    proc_->stop.store(true);
    {
        std::lock_guard<std::mutex> lock(proc_->mu);
        proc_->handler = nullptr;
    }
    if (proc_->wake_w >= 0) {
        const char b = 'x';
        (void)!write(proc_->wake_w, &b, 1);
    }
    if (proc_->thread.joinable()) {
        proc_->thread.join();
    }
    if (proc_->pid > 0 && !proc_->reaped.load()) {
        kill(-proc_->pid, SIGTERM);
        int status = 0;
        waitpid(proc_->pid, &status, 0);
        proc_->status = status;
        proc_->reaped.store(true);
    }
    CloseFd(&proc_->in_fd);
    CloseFd(&proc_->out_fd);
    CloseFd(&proc_->err_fd);
    CloseFd(&proc_->wake_r);
    CloseFd(&proc_->wake_w);
    delete proc_;
    proc_ = nullptr;
}

void LspClient::Enqueue(const std::string& json) {
    if (proc_ == nullptr || proc_->reaped.load() || proc_->stop.load()) {
        return;
    }
    const std::string frame = EncodeFrame(json);
    {
        std::lock_guard<std::mutex> lock(proc_->mu);
        if (proc_->ready.load()) {
            proc_->queued.append(frame);
        } else {
            proc_->pending.append(frame);
        }
    }
    if (proc_->wake_w >= 0) {
        const char b = 'x';
        (void)!write(proc_->wake_w, &b, 1);
    }
}

void LspClient::DidOpen(const std::string& path, const std::string& language, int64_t version,
                        const std::string& text) {
    Value doc = JsonObject();
    Put(doc, "uri", JsonString(PathToUri(path)));
    Put(doc, "languageId", JsonString(language));
    Put(doc, "version", JsonInt(version));
    Put(doc, "text", JsonString(text));
    Value params = JsonObject();
    Put(params, "textDocument", std::move(doc));
    Value msg = JsonObject();
    Put(msg, "jsonrpc", JsonString("2.0"));
    Put(msg, "method", JsonString("textDocument/didOpen"));
    Put(msg, "params", std::move(params));
    Enqueue(Stringify(msg));
}

void LspClient::DidChange(const std::string& path, int64_t version, Pos start, Pos end,
                          const std::string& text, const std::string& full) {
    Value change = JsonObject();
    if (proc_ != nullptr && proc_->sync_kind.load() == 1) {
        Put(change, "text", JsonString(full));
    } else {
        Put(change, "range", RangeOf(start, end));
        Put(change, "text", JsonString(text));
    }
    Value changes = JsonArray();
    Push(changes, std::move(change));
    Value ident = JsonObject();
    Put(ident, "uri", JsonString(PathToUri(path)));
    Put(ident, "version", JsonInt(version));
    Value params = JsonObject();
    Put(params, "textDocument", std::move(ident));
    Put(params, "contentChanges", std::move(changes));
    Value msg = JsonObject();
    Put(msg, "jsonrpc", JsonString("2.0"));
    Put(msg, "method", JsonString("textDocument/didChange"));
    Put(msg, "params", std::move(params));
    Enqueue(Stringify(msg));
}

void LspClient::DidChangeFull(const std::string& path, int64_t version, const std::string& text) {
    Value change = JsonObject();
    Put(change, "text", JsonString(text));
    Value changes = JsonArray();
    Push(changes, std::move(change));
    Value ident = JsonObject();
    Put(ident, "uri", JsonString(PathToUri(path)));
    Put(ident, "version", JsonInt(version));
    Value params = JsonObject();
    Put(params, "textDocument", std::move(ident));
    Put(params, "contentChanges", std::move(changes));
    Value msg = JsonObject();
    Put(msg, "jsonrpc", JsonString("2.0"));
    Put(msg, "method", JsonString("textDocument/didChange"));
    Put(msg, "params", std::move(params));
    Enqueue(Stringify(msg));
}

void LspClient::DidSave(const std::string& path, const std::string& text) {
    Value ident = JsonObject();
    Put(ident, "uri", JsonString(PathToUri(path)));
    Value params = JsonObject();
    Put(params, "textDocument", std::move(ident));
    Put(params, "text", JsonString(text));
    Value msg = JsonObject();
    Put(msg, "jsonrpc", JsonString("2.0"));
    Put(msg, "method", JsonString("textDocument/didSave"));
    Put(msg, "params", std::move(params));
    Enqueue(Stringify(msg));
}

void LspClient::DidClose(const std::string& path) {
    Value ident = JsonObject();
    Put(ident, "uri", JsonString(PathToUri(path)));
    Value params = JsonObject();
    Put(params, "textDocument", std::move(ident));
    Value msg = JsonObject();
    Put(msg, "jsonrpc", JsonString("2.0"));
    Put(msg, "method", JsonString("textDocument/didClose"));
    Put(msg, "params", std::move(params));
    Enqueue(Stringify(msg));
}

void LspClient::Completion(const std::string& path, Pos pos, int trigger_kind, char trigger) {
    if (proc_ == nullptr) {
        return;
    }
    const int64_t id = proc_->next_id.fetch_add(1);
    proc_->completion_id.store(id);
    Value position = JsonObject();
    Put(position, "line", JsonInt(pos.line));
    Put(position, "character", JsonInt(pos.character));
    Value context = JsonObject();
    Put(context, "triggerKind", JsonInt(trigger_kind));
    if (trigger_kind == 2) {
        Put(context, "triggerCharacter", JsonString(std::string(1, trigger)));
    }
    Value doc = JsonObject();
    Put(doc, "uri", JsonString(PathToUri(path)));
    Value params = JsonObject();
    Put(params, "textDocument", std::move(doc));
    Put(params, "position", std::move(position));
    Put(params, "context", std::move(context));
    Value msg = JsonObject();
    Put(msg, "jsonrpc", JsonString("2.0"));
    Put(msg, "id", JsonInt(id));
    Put(msg, "method", JsonString("textDocument/completion"));
    Put(msg, "params", std::move(params));
    Enqueue(Stringify(msg));
}

void LspClient::Hover(const std::string& path, Pos pos) {
    if (proc_ == nullptr) {
        return;
    }
    const int64_t id = proc_->next_id.fetch_add(1);
    proc_->hover_id.store(id);
    Value position = JsonObject();
    Put(position, "line", JsonInt(pos.line));
    Put(position, "character", JsonInt(pos.character));
    Value doc = JsonObject();
    Put(doc, "uri", JsonString(PathToUri(path)));
    Value params = JsonObject();
    Put(params, "textDocument", std::move(doc));
    Put(params, "position", std::move(position));
    Value msg = JsonObject();
    Put(msg, "jsonrpc", JsonString("2.0"));
    Put(msg, "id", JsonInt(id));
    Put(msg, "method", JsonString("textDocument/hover"));
    Put(msg, "params", std::move(params));
    Enqueue(Stringify(msg));
}

void LspClient::Definition(const std::string& path, Pos pos) {
    if (proc_ == nullptr) {
        return;
    }
    const int64_t id = proc_->next_id.fetch_add(1);
    proc_->definition_id.store(id);
    Value position = JsonObject();
    Put(position, "line", JsonInt(pos.line));
    Put(position, "character", JsonInt(pos.character));
    Value doc = JsonObject();
    Put(doc, "uri", JsonString(PathToUri(path)));
    Value params = JsonObject();
    Put(params, "textDocument", std::move(doc));
    Put(params, "position", std::move(position));
    Value msg = JsonObject();
    Put(msg, "jsonrpc", JsonString("2.0"));
    Put(msg, "id", JsonInt(id));
    Put(msg, "method", JsonString("textDocument/definition"));
    Put(msg, "params", std::move(params));
    Enqueue(Stringify(msg));
}

void LspClient::Loop() {
    Process* proc = proc_;
    std::string incoming;
    std::string outgoing;
    bool dead = false;

    auto dispatch = [&](auto&& call) {
        Handler* handler = nullptr;
        {
            std::lock_guard<std::mutex> lock(proc->mu);
            handler = proc->stop.load() ? nullptr : proc->handler;
        }
        if (handler != nullptr) {
            call(handler);
        }
    };

    auto handle = [&](const std::string& body) {
        Value msg;
        std::string perr;
        if (!Parse(body, &msg, &perr)) {
            return;
        }
        const Value* method = msg.find("method");
        const Value* id = msg.find("id");
        const Value* result = msg.find("result");
        if (method != nullptr && method->kind == Value::Kind::String &&
            method->s == "textDocument/publishDiagnostics") {
            const Value* params = msg.find("params");
            if (params == nullptr) {
                return;
            }
            const Value* uri = params->find("uri");
            const Value* list = params->find("diagnostics");
            if (uri == nullptr || uri->kind != Value::Kind::String || list == nullptr ||
                list->kind != Value::Kind::Array) {
                return;
            }
            std::vector<Diagnostic> diags;
            for (const Value& item : list->a) {
                Diagnostic diag;
                diag.path = UriToPath(uri->s);
                const Value* range = item.find("range");
                Pos start;
                Pos end;
                if (!ReadPos(range, "start", &start) || !ReadPos(range, "end", &end)) {
                    continue;
                }
                diag.line = start.line;
                diag.character = start.character;
                diag.end_line = end.line;
                diag.end_character = end.character;
                if (const Value* severity = item.find("severity")) {
                    if (severity->is_int) {
                        diag.severity = static_cast<int>(severity->i);
                    }
                }
                if (const Value* text = item.find("message")) {
                    if (text->kind == Value::Kind::String) {
                        diag.message = text->s;
                    }
                }
                diags.push_back(std::move(diag));
            }
            const std::string path = UriToPath(uri->s);
            dispatch([&](Handler* handler) { handler->OnDiagnostics(path, std::move(diags)); });
            return;
        }
        if (id != nullptr && id->is_int && result != nullptr) {
            if (!proc->ready.load() && id->i == 1) {
                int kind = 2;
                if (const Value* caps = result->find("capabilities")) {
                    if (const Value* sync = caps->find("textDocumentSync")) {
                        if (sync->is_int) {
                            kind = static_cast<int>(sync->i);
                        } else if (const Value* change = sync->find("change")) {
                            if (change->is_int) {
                                kind = static_cast<int>(change->i);
                            }
                        }
                    }
                }
                if (kind != 1) {
                    kind = 2;
                }
                proc->sync_kind.store(kind);
                Value note = JsonObject();
                Put(note, "jsonrpc", JsonString("2.0"));
                Put(note, "method", JsonString("initialized"));
                Put(note, "params", JsonObject());
                {
                    std::lock_guard<std::mutex> lock(proc->mu);
                    proc->queued.insert(0, EncodeFrame(Stringify(note)) + proc->pending);
                    proc->pending.clear();
                    proc->ready.store(true);
                }
                dispatch([&](Handler* handler) { handler->OnNote(proc->command); });
                return;
            }
            if (id->i == proc->completion_id.load()) {
                std::vector<CompletionItem> items;
                const Value* list = result;
                if (result->kind == Value::Kind::Object) {
                    if (const Value* inner = result->find("items")) {
                        list = inner;
                    }
                }
                if (list->kind == Value::Kind::Array) {
                    for (const Value& item : list->a) {
                        CompletionItem done;
                        if (const Value* label = item.find("label")) {
                            if (label->kind == Value::Kind::String) {
                                done.label = label->s;
                            }
                        }
                        done.insert = done.label;
                        if (const Value* insert = item.find("insertText")) {
                            if (insert->kind == Value::Kind::String) {
                                done.insert = insert->s;
                            }
                        }
                        const Value* edit = item.find("textEdit");
                        if (edit != nullptr) {
                            if (const Value* text = edit->find("newText")) {
                                if (text->kind == Value::Kind::String) {
                                    done.insert = text->s;
                                }
                            }
                            const Value* range = edit->find("range");
                            if (range == nullptr) {
                                range = edit->find("replace");
                            }
                            Pos start;
                            Pos end;
                            if (ReadPos(range, "start", &start) && ReadPos(range, "end", &end)) {
                                done.has_range = true;
                                done.start_line = start.line;
                                done.start_character = start.character;
                                done.end_line = end.line;
                                done.end_character = end.character;
                            }
                        }
                        const size_t dollar = done.insert.find('$');
                        if (dollar != std::string::npos) {
                            done.insert.resize(dollar);
                        }
                        if (!done.label.empty()) {
                            items.push_back(std::move(done));
                        }
                    }
                }
                dispatch([&](Handler* handler) { handler->OnCompletion(std::move(items)); });
                return;
            }
            if (id->i == proc->hover_id.load()) {
                std::string text;
                if (result->kind != Value::Kind::Null) {
                    if (const Value* contents = result->find("contents")) {
                        text = HoverText(*contents);
                    }
                }
                dispatch([&](Handler* handler) { handler->OnHover(std::move(text)); });
                return;
            }
            if (id->i == proc->definition_id.load()) {
                std::vector<Location> locations;
                auto add_one = [&](const Value& item) {
                    Location loc;
                    const Value* uri = item.find("uri");
                    const Value* range = item.find("range");
                    if (uri == nullptr) {
                        uri = item.find("targetUri");
                        range = item.find("targetSelectionRange");
                        if (range == nullptr) {
                            range = item.find("targetRange");
                        }
                    }
                    if (uri == nullptr || uri->kind != Value::Kind::String) {
                        return;
                    }
                    Pos start;
                    if (!ReadPos(range, "start", &start)) {
                        return;
                    }
                    loc.path = UriToPath(uri->s);
                    loc.line = start.line;
                    loc.character = start.character;
                    locations.push_back(std::move(loc));
                };
                if (result->kind == Value::Kind::Array) {
                    for (const Value& item : result->a) {
                        add_one(item);
                    }
                } else if (result->kind == Value::Kind::Object) {
                    add_one(*result);
                }
                dispatch([&](Handler* handler) { handler->OnDefinition(std::move(locations)); });
            }
        }
    };

    while (!dead) {
        {
            std::lock_guard<std::mutex> lock(proc->mu);
            if (!proc->queued.empty()) {
                outgoing.append(proc->queued);
                proc->queued.clear();
            }
        }
        pollfd fds[4];
        fds[0].fd = proc->out_fd;
        fds[0].events = POLLIN;
        fds[0].revents = 0;
        fds[1].fd = proc->err_fd;
        fds[1].events = POLLIN;
        fds[1].revents = 0;
        fds[2].fd = proc->wake_r;
        fds[2].events = POLLIN;
        fds[2].revents = 0;
        nfds_t count = 3;
        if (!outgoing.empty() && proc->in_fd >= 0) {
            fds[3].fd = proc->in_fd;
            fds[3].events = POLLOUT;
            fds[3].revents = 0;
            count = 4;
        }
        const int pr = poll(fds, count, 200);
        if (pr < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (count == 4 && (fds[3].revents & (POLLOUT | POLLERR | POLLHUP)) != 0) {
            if ((fds[3].revents & POLLOUT) != 0) {
                const ssize_t wrote = write(proc->in_fd, outgoing.data(), outgoing.size());
                if (wrote > 0) {
                    outgoing.erase(0, static_cast<size_t>(wrote));
                } else if (wrote < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                    outgoing.clear();
                }
            } else {
                outgoing.clear();
            }
        }
        if ((fds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            char buf[4096];
            const ssize_t got = read(proc->out_fd, buf, sizeof(buf));
            if (got > 0) {
                incoming.append(buf, static_cast<size_t>(got));
                for (;;) {
                    std::string body;
                    std::string ferr;
                    if (!TakeFrame(&incoming, &body, &ferr)) {
                        if (!ferr.empty()) {
                            dead = true;
                        }
                        break;
                    }
                    handle(body);
                }
            } else if (got == 0 || (got < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                dead = true;
            }
        }
        if ((fds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            char buf[1024];
            const ssize_t got = read(proc->err_fd, buf, sizeof(buf));
            if (got > 0) {
                proc->stderr_tail.append(buf, static_cast<size_t>(got));
                if (proc->stderr_tail.size() > 2000) {
                    proc->stderr_tail.erase(0, proc->stderr_tail.size() - 2000);
                }
            }
        }
        if ((fds[2].revents & POLLIN) != 0) {
            char buf[64];
            while (read(proc->wake_r, buf, sizeof(buf)) > 0) {
            }
        }
        if (proc->stop.load() && outgoing.empty()) {
            break;
        }
    }

    if (proc->pid > 0 && !proc->reaped.exchange(true)) {
        int status = 0;
        if (waitpid(proc->pid, &status, WNOHANG) == 0) {
            kill(-proc->pid, SIGTERM);
            waitpid(proc->pid, &status, 0);
        }
        proc->status = status;
    }
    if (!proc->stop.load()) {
        std::string message = proc->command;
        if (WIFEXITED(proc->status)) {
            message += " exited (" + std::to_string(WEXITSTATUS(proc->status)) + ")";
        } else if (WIFSIGNALED(proc->status)) {
            message += " killed (" + std::to_string(WTERMSIG(proc->status)) + ")";
        } else {
            message += " stopped";
        }
        size_t end = proc->stderr_tail.size();
        while (end > 0 && (proc->stderr_tail[end - 1] == '\n' || proc->stderr_tail[end - 1] == '\r')) {
            --end;
        }
        size_t start = 0;
        if (end > 0) {
            const size_t nl = proc->stderr_tail.rfind('\n', end - 1);
            start = nl == std::string::npos ? 0 : nl + 1;
            if (start < end && proc->stderr_tail[start] == '\r') {
                ++start;
            }
        }
        if (end > start) {
            if (end - start > 80) {
                end = start + 80;
            }
            message += ": ";
            message.append(proc->stderr_tail, start, end - start);
        }
        dispatch([&](Handler* handler) { handler->OnServerExit(message); });
    }
}

}  // namespace haikuide
