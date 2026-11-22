/* Pipes to node host/dist/main.js. Same rules as the clangd client:
 * non-blocking writes, a wake pipe, a private process group, SIGPIPE ignored.
 *
 * Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
 */
#include "host_link.hpp"

#include "protocol.hpp"
#include "rpc.hpp"

#include <Messenger.h>
#include <Window.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace haikuide {
namespace {

const uint32 kHostCompletion = 0x48000110;
const uint32 kHostExit = 0x48000111;
const uint32 kHostFailed = 0x48000112;
const uint32 kHostEdit = 0x48000113;
const uint32 kHostDiagnostics = 0x48000114;

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

bool IsFile(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

std::string LanguageOf(const std::string& path) {
    const size_t dot = path.rfind('.');
    std::string ext = dot == std::string::npos ? std::string() : path.substr(dot);
    for (char& ch : ext) {
        if (ch >= 'A' && ch <= 'Z') {
            ch = static_cast<char>(ch - 'A' + 'a');
        }
    }
    if (ext == ".c") {
        return "c";
    }
    if (ext == ".cc" || ext == ".cpp" || ext == ".cxx" || ext == ".h" || ext == ".hh" || ext == ".hpp" ||
        ext == ".hxx") {
        return "cpp";
    }
    return "plaintext";
}

int JsonIntOf(const Value* value, int fallback) {
    if (value != nullptr && value->is_int) {
        return static_cast<int>(value->i);
    }
    return fallback;
}

std::string JsonStringOf(const Value* value) {
    if (value != nullptr && value->kind == Value::Kind::String) {
        return value->s;
    }
    return std::string();
}

std::vector<CompletionItem> ItemsFrom(const Value* list) {
    std::vector<CompletionItem> items;
    if (list == nullptr || list->kind != Value::Kind::Array) {
        return items;
    }
    for (const Value& item : list->a) {
        CompletionItem done;
        done.label = JsonStringOf(item.find("label"));
        done.insert = JsonStringOf(item.find("insertText"));
        if (done.insert.empty()) {
            done.insert = done.label;
        }
        const size_t dollar = done.insert.find('$');
        if (dollar != std::string::npos) {
            done.insert.resize(dollar);
        }
        if (!done.label.empty()) {
            items.push_back(std::move(done));
        }
    }
    return items;
}

std::string TailLine(const std::string& tail) {
    size_t end = tail.size();
    while (end > 0 && (tail[end - 1] == '\n' || tail[end - 1] == '\r')) {
        --end;
    }
    size_t start = 0;
    if (end > 0) {
        const size_t nl = tail.rfind('\n', end - 1);
        start = nl == std::string::npos ? 0 : nl + 1;
        if (start < end && tail[start] == '\r') {
            ++start;
        }
    }
    if (end <= start) {
        return std::string();
    }
    if (end - start > 80) {
        end = start + 80;
    }
    return tail.substr(start, end - start);
}

}  // namespace

struct HostLink::Process {
    std::mutex mu;
    std::string queued;
    std::map<int64_t, uint64_t> epochs;
    std::atomic<bool> stop{false};
    std::atomic<bool> reaped{false};
    std::atomic<int64_t> next_id{1};
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

HostLink::HostLink(BWindow* window, LspBridge* lsp, LspSurface* surface)
    : window_(window), lsp_(lsp), surface_(surface) {}

bool HostLink::running() const { return proc_ != nullptr && !proc_->reaped.load(); }

void HostLink::Post(BMessage* message) {
    if (dead_) {
        delete message;
        return;
    }
    BMessenger(this).SendMessage(message);
    delete message;
}

bool HostLink::Start(const std::string& script, std::string* err) {
    Shutdown();
    dead_ = false;
    if (!IsFile(script)) {
        *err = "extension host not installed";
        return false;
    }
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

    const char* command = "node";
    std::string script_copy = script;
    char* argv[] = {const_cast<char*>(command), script_copy.data(), nullptr};
    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, command, &actions, &attr, argv, environ);
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
        *err = std::string("cannot start node: ") + std::strerror(rc);
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
    proc_->in_fd = child_in[1];
    proc_->out_fd = child_out[0];
    proc_->err_fd = child_err[0];
    proc_->wake_r = wake[0];
    proc_->wake_w = wake[1];
    proc_->pid = pid;
    proc_->thread = std::thread([this] { Loop(); });

    const char* home = getenv("HOME");
    const std::string root = home != nullptr && home[0] != '\0' ? home : "/boot/home";
    Value params = JsonObject();
    Put(params, "rootUri", JsonString(PathToUri(root)));
    Put(params, "rootPath", JsonString(root));
    Request("workspace/open", std::move(params), 0);
    if (lsp_ != nullptr) {
        lsp_->SetHostAttached(true);
    }
    return true;
}

void HostLink::Shutdown() {
    if (proc_ == nullptr) {
        return;
    }
    dead_ = true;
    proc_->stop.store(true);
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
        proc_->reaped.store(true);
    }
    CloseFd(&proc_->in_fd);
    CloseFd(&proc_->out_fd);
    CloseFd(&proc_->err_fd);
    CloseFd(&proc_->wake_r);
    CloseFd(&proc_->wake_w);
    delete proc_;
    proc_ = nullptr;
    opened_.clear();
}

void HostLink::Enqueue(const std::string& json) {
    if (proc_ == nullptr || proc_->reaped.load() || proc_->stop.load()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(proc_->mu);
        proc_->queued.append(EncodeFrame(json));
    }
    if (proc_->wake_w >= 0) {
        const char b = 'x';
        (void)!write(proc_->wake_w, &b, 1);
    }
}

void HostLink::Request(const std::string& method, Value params, uint64_t epoch) {
    if (proc_ == nullptr) {
        return;
    }
    const int64_t id = proc_->next_id.fetch_add(1);
    if (epoch != 0) {
        std::lock_guard<std::mutex> lock(proc_->mu);
        proc_->epochs[id] = epoch;
    }
    Value msg = JsonObject();
    Put(msg, "jsonrpc", JsonString("2.0"));
    Put(msg, "id", JsonInt(id));
    Put(msg, "method", JsonString(method));
    Put(msg, "params", std::move(params));
    Enqueue(Stringify(msg));
}

void HostLink::Open(EditorTab* tab) {
    if (tab == nullptr || tab->path.empty() || proc_ == nullptr) {
        return;
    }
    if (std::find(opened_.begin(), opened_.end(), tab->path) != opened_.end()) {
        return;
    }
    opened_.push_back(tab->path);
    Value params = JsonObject();
    Put(params, "uri", JsonString(PathToUri(tab->path)));
    Put(params, "languageId", JsonString(LanguageOf(tab->path)));
    Put(params, "version", JsonInt(static_cast<int64_t>(tab->doc.version())));
    Put(params, "text", JsonString(tab->flat));
    Request("doc/didOpen", std::move(params), 0);
}

void HostLink::NoteEdit() { edit_pending_ = true; }

void HostLink::FlushEdit(EditorTab* tab) {
    if (!edit_pending_ || tab == nullptr) {
        return;
    }
    edit_pending_ = false;
    if (tab->path.empty()) {
        return;
    }
    if (std::find(opened_.begin(), opened_.end(), tab->path) == opened_.end()) {
        Open(tab);
        return;
    }
    Value params = JsonObject();
    Put(params, "uri", JsonString(PathToUri(tab->path)));
    Put(params, "version", JsonInt(static_cast<int64_t>(tab->doc.version())));
    Put(params, "text", JsonString(tab->flat));
    Request("doc/didChange", std::move(params), 0);
}

void HostLink::FullSync(EditorTab* tab) {
    edit_pending_ = false;
    if (tab == nullptr || tab->path.empty()) {
        return;
    }
    if (std::find(opened_.begin(), opened_.end(), tab->path) == opened_.end()) {
        Open(tab);
        return;
    }
    Value params = JsonObject();
    Put(params, "uri", JsonString(PathToUri(tab->path)));
    Put(params, "version", JsonInt(static_cast<int64_t>(tab->doc.version())));
    Put(params, "text", JsonString(tab->flat));
    Request("doc/didChange", std::move(params), 0);
}

void HostLink::Save(EditorTab* tab) {
    if (tab == nullptr || tab->path.empty()) {
        return;
    }
    Open(tab);
    Value params = JsonObject();
    Put(params, "uri", JsonString(PathToUri(tab->path)));
    Put(params, "text", JsonString(tab->flat));
    Request("doc/didSave", std::move(params), 0);
}

void HostLink::Close(const std::string& path) {
    auto found = std::find(opened_.begin(), opened_.end(), path);
    if (found == opened_.end()) {
        return;
    }
    opened_.erase(found);
    Value params = JsonObject();
    Put(params, "uri", JsonString(PathToUri(path)));
    Request("doc/didClose", std::move(params), 0);
}

void HostLink::Complete(EditorTab* tab, uint64_t epoch, int kind, char trigger) {
    if (tab == nullptr || tab->path.empty() || proc_ == nullptr || epoch == 0) {
        return;
    }
    Open(tab);
    Value params = JsonObject();
    Put(params, "uri", JsonString(PathToUri(tab->path)));
    const Pos pos = ByteToPos(tab->flat, tab->caret);
    Put(params, "line", JsonInt(pos.line));
    Put(params, "character", JsonInt(pos.character));
    Put(params, "triggerKind", JsonInt(kind));
    if (kind == 2 && trigger != 0) {
        Put(params, "triggerCharacter", JsonString(std::string(1, trigger)));
    }
    Request("completion/request", std::move(params), epoch);
}

void HostLink::Loop() {
    Process* proc = proc_;
    std::string incoming;
    std::string outgoing;
    bool dead = false;

    auto handle = [&](const std::string& body) {
        Value msg;
        std::string perr;
        if (!Parse(body, &msg, &perr)) {
            return;
        }
        const Value* method = msg.find("method");
        const Value* id = msg.find("id");
        const std::string name = JsonStringOf(method);
        if (!name.empty()) {
            const Value* params = msg.find("params");
            if (name == "completion/result" && params != nullptr) {
                const int64_t request = params->find("id") != nullptr && params->find("id")->is_int
                                            ? params->find("id")->i
                                            : 0;
                uint64_t epoch = 0;
                {
                    std::lock_guard<std::mutex> lock(proc->mu);
                    auto found = proc->epochs.find(request);
                    if (found != proc->epochs.end()) {
                        epoch = found->second;
                        proc->epochs.erase(found);
                    }
                }
                if (epoch == 0) {
                    return;
                }
                BMessage* message = new BMessage(kHostCompletion);
                message->AddInt64("epoch", static_cast<int64_t>(epoch));
                for (const CompletionItem& item : ItemsFrom(params->find("items"))) {
                    message->AddString("label", item.label.c_str());
                    message->AddString("insert", item.insert.c_str());
                }
                Post(message);
                return;
            }
            if (name == "diagnostics/set" && params != nullptr) {
                const std::string path = JsonStringOf(params->find("path"));
                BMessage* message = new BMessage(kHostDiagnostics);
                message->AddString("path", path.c_str());
                const Value* list = params->find("diagnostics");
                if (list != nullptr && list->kind == Value::Kind::Array) {
                    for (const Value& item : list->a) {
                        const Value* range = item.find("range");
                        int line = 0;
                        int character = 0;
                        int end_line = 0;
                        int end_character = 0;
                        if (range != nullptr) {
                            const Value* start = range->find("start");
                            const Value* end = range->find("end");
                            if (start != nullptr) {
                                line = JsonIntOf(start->find("line"), 0);
                                character = JsonIntOf(start->find("character"), 0);
                            }
                            if (end != nullptr) {
                                end_line = JsonIntOf(end->find("line"), line);
                                end_character = JsonIntOf(end->find("character"), character);
                            }
                        }
                        message->AddInt32("line", line);
                        message->AddInt32("character", character);
                        message->AddInt32("end_line", end_line);
                        message->AddInt32("end_character", end_character);
                        message->AddInt32("severity", JsonIntOf(item.find("severity"), 1));
                        message->AddString("message", JsonStringOf(item.find("message")).c_str());
                    }
                }
                Post(message);
                return;
            }
            if (name == "doc/applyEdit" && params != nullptr) {
                BMessage* message = new BMessage(kHostEdit);
                message->AddString("path", JsonStringOf(params->find("path")).c_str());
                message->AddInt64("pos", params->find("pos") && params->find("pos")->is_int ? params->find("pos")->i : 0);
                message->AddInt64("len", params->find("len") && params->find("len")->is_int ? params->find("len")->i : 0);
                message->AddString("text", JsonStringOf(params->find("text")).c_str());
                Post(message);
                return;
            }
            if (name == "host/extensionFailed" && params != nullptr) {
                BMessage* message = new BMessage(kHostFailed);
                const std::string text = JsonStringOf(params->find("id")) + " failed";
                message->AddString("text", text.c_str());
                Post(message);
                return;
            }
            return;
        }
        if (id != nullptr && id->is_int) {
            const Value* result = msg.find("result");
            uint64_t epoch = 0;
            {
                std::lock_guard<std::mutex> lock(proc->mu);
                auto found = proc->epochs.find(id->i);
                if (found == proc->epochs.end()) {
                    return;
                }
                epoch = found->second;
                proc->epochs.erase(found);
            }
            if (result == nullptr || epoch == 0) {
                return;
            }
            BMessage* message = new BMessage(kHostCompletion);
            message->AddInt64("epoch", static_cast<int64_t>(epoch));
            for (const CompletionItem& item : ItemsFrom(result->find("items"))) {
                message->AddString("label", item.label.c_str());
                message->AddString("insert", item.insert.c_str());
            }
            Post(message);
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
        std::string message = "node";
        if (WIFEXITED(proc->status)) {
            message += " exited (" + std::to_string(WEXITSTATUS(proc->status)) + ")";
        } else if (WIFSIGNALED(proc->status)) {
            message += " killed (" + std::to_string(WTERMSIG(proc->status)) + ")";
        } else {
            message += " stopped";
        }
        const std::string tail = TailLine(proc->stderr_tail);
        if (!tail.empty()) {
            message += ": ";
            message += tail;
        }
        BMessage* note = new BMessage(kHostExit);
        note->AddString("text", message.c_str());
        Post(note);
    }
}

void HostLink::MessageReceived(BMessage* message) {
    if (dead_ || message == nullptr || lsp_ == nullptr) {
        return;
    }
    switch (message->what) {
    case kHostCompletion: {
        int64 epoch = 0;
        if (message->FindInt64("epoch", &epoch) != B_OK) {
            break;
        }
        std::vector<CompletionItem> items;
        const char* label = nullptr;
        for (int32 i = 0; message->FindString("label", i, &label) == B_OK; ++i) {
            CompletionItem item;
            item.label = label != nullptr ? label : "";
            const char* insert = nullptr;
            if (message->FindString("insert", i, &insert) == B_OK && insert != nullptr) {
                item.insert = insert;
            }
            if (!item.label.empty()) {
                items.push_back(std::move(item));
            }
        }
        lsp_->OfferHost(static_cast<uint64_t>(epoch), std::move(items));
        break;
    }
    case kHostExit:
    case kHostFailed: {
        const char* text = nullptr;
        std::string message_text = "extension host stopped";
        if (message->FindString("text", &text) == B_OK && text != nullptr) {
            message_text = text;
        }
        lsp_->HostDown(message_text);
        break;
    }
    case kHostDiagnostics: {
        const char* path = nullptr;
        if (message->FindString("path", &path) != B_OK || path == nullptr) {
            break;
        }
        std::vector<Diagnostic> diags;
        int32 line = 0;
        for (int32 i = 0; message->FindInt32("line", i, &line) == B_OK; ++i) {
            Diagnostic diag;
            diag.path = path;
            diag.line = line;
            int32 value = 0;
            if (message->FindInt32("character", i, &value) == B_OK) {
                diag.character = value;
            }
            if (message->FindInt32("end_line", i, &value) == B_OK) {
                diag.end_line = value;
            }
            if (message->FindInt32("end_character", i, &value) == B_OK) {
                diag.end_character = value;
            }
            if (message->FindInt32("severity", i, &value) == B_OK) {
                diag.severity = value;
            }
            const char* text = nullptr;
            if (message->FindString("message", i, &text) == B_OK && text != nullptr) {
                diag.message = text;
            }
            diags.push_back(std::move(diag));
        }
        lsp_->HostDiagnostics(path, std::move(diags));
        break;
    }
    case kHostEdit: {
        const char* path = nullptr;
        const char* text = nullptr;
        int64 pos = 0;
        int64 len = 0;
        if (message->FindString("path", &path) != B_OK || path == nullptr || surface_ == nullptr) {
            break;
        }
        message->FindInt64("pos", &pos);
        message->FindInt64("len", &len);
        if (message->FindString("text", &text) != B_OK || text == nullptr) {
            text = "";
        }
        if (pos < 0) {
            pos = 0;
        }
        if (len < 0) {
            len = 0;
        }
        surface_->ApplyEdit(static_cast<size_t>(pos), static_cast<size_t>(len), text);
        break;
    }
    default:
        BHandler::MessageReceived(message);
        break;
    }
}

}  // namespace haikuide
