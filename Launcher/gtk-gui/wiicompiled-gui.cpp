// a minimal GTK3 frontend
#include <gtk/gtk.h>

#include <cstdlib>
#include <sstream>
#include <string>
#include <vector>

namespace {

const char *const APP_ID = "io.github.TeamWheelWizard.Wiicompiled";

enum class GameStatus { Unknown, Missing, Current, Stale };
enum class OpKind { Check, Install, Play };

// Minimal JSON value extraction for the flat --progress-json lines-
std::string json_string(const std::string &line, const std::string &key) {
    const std::string pat = "\"" + key + "\":\"";
    size_t p = line.find(pat);
    if (p == std::string::npos)
        return {};
    p += pat.size();
    std::string out;
    while (p < line.size() && line[p] != '"') {
        if (line[p] == '\\' && p + 1 < line.size()) {
            p++;
            switch (line[p]) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'u':
                if (p + 4 < line.size()) {
                    char hex[5] = {line[p + 1], line[p + 2], line[p + 3], line[p + 4], 0};
                    gunichar c = (gunichar)std::strtoul(hex, nullptr, 16);
                    char utf8[7] = {0};
                    g_unichar_to_utf8(c, utf8);
                    out += utf8;
                    p += 4;
                }
                break;
            default: out += line[p]; break;
            }
        } else {
            out += line[p];
        }
        p++;
    }
    return out;
}

long json_int(const std::string &line, const std::string &key) {
    const std::string pat = "\"" + key + "\":";
    const size_t p = line.find(pat);
    return p == std::string::npos ? -1 : std::strtol(line.c_str() + p + pat.size(), nullptr, 10);
}

bool json_is_true(const std::string &line, const std::string &key) {
    return line.find("\"" + key + "\":true") != std::string::npos;
}

void trim(std::string &s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) {
        s.clear();
        return;
    }
    const size_t e = s.find_last_not_of(" \t\r\n");
    s = s.substr(b, e - b + 1);
}

class Gui;

// One Operation: a wiicompiled-setup subprocess whose stdout/stderr are read
// line by line and whose exit is awaited, all asynchronously. The three GIO
// callbacks each notify completion; when all three fire, finish() runs the
// operation-specific outcome handling and the Op deletes itself (the Gui
// outlives every Op).
class Op {
public:
    Gui *gui;
    OpKind kind;
    GSubprocess *sub = nullptr;
    GDataInputStream *out = nullptr;
    GDataInputStream *err = nullptr;
    std::string check_out;
    bool saw_result = false;
    int done_parts = 0;

    Op(Gui *gui, OpKind kind) : gui(gui), kind(kind) {}
    ~Op() {
        g_clear_object(&sub);
        g_clear_object(&out);
        g_clear_object(&err);
    }

    void part_done();
    void finish();
    void handle_progress_line(const std::string &line);

    static void on_read_out(GObject *src, GAsyncResult *res, gpointer data);
    static void on_read_err(GObject *src, GAsyncResult *res, gpointer data);
    static void on_wait(GObject *src, GAsyncResult *res, gpointer data);
};

class Gui {
public:
    GtkApplication *app = nullptr;
    GtkWidget *window = nullptr;
    GtkWidget *root = nullptr;
    GtkWidget *status_label = nullptr;
    GtkWidget *stage_label = nullptr;
    GtkWidget *progress = nullptr;
    GtkWidget *log_view = nullptr;
    GtkTextBuffer *log_buf = nullptr;
    GtkWidget *play_btn = nullptr;
    GtkWidget *install_btn = nullptr;
    GameStatus base_status = GameStatus::Unknown;

    explicit Gui(GtkApplication *app) : app(app) { build_ui(); }

    void log(const std::string &line);
    void show_error(const std::string &msg);
    void refresh_status_row();
    void set_busy(bool busy);
    void run_check();
    bool spawn(OpKind kind, std::vector<std::string> args);
    void parse_check_output(const std::string &output);

    // The GApplication callback and the two button handlers are reached
    // through C function-pointer casts, so they cannot be private.
    static void activate(GtkApplication *app, gpointer data);
    static void on_play(GtkButton *btn, gpointer data);
    static void on_install(GtkButton *btn, gpointer data);

private:
    void build_ui();
    void build_controls_row();
    void build_status_row();
    void build_log_pane();

    static std::string find_setup();
};

// --- Op ------------------------------------------------------------------

void Op::handle_progress_line(const std::string &line) {
    if (line.find("\"type\":\"progress\"") != std::string::npos) {
        const long pct = json_int(line, "percent");
        std::string msg = json_string(line, "message");
        if (pct >= 0 && pct <= 100)
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(gui->progress), pct / 100.0);
        gtk_label_set_text(GTK_LABEL(gui->stage_label), msg.c_str());
    } else if (line.find("\"type\":\"result\"") != std::string::npos) {
        saw_result = true;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(gui->progress), 1.0);
        if (json_is_true(line, "success")) {
            std::string msg = "Install complete";
            const std::string dir = json_string(line, "installDir");
            if (!dir.empty())
                msg += ": " + dir;
            gui->log(msg);
            gtk_label_set_text(GTK_LABEL(gui->stage_label), msg.c_str());
        } else {
            const std::string err = json_string(line, "error");
            const std::string msg = "Install failed: " + (err.empty() ? "unknown error" : err);
            gui->log(msg);
            gtk_label_set_text(GTK_LABEL(gui->stage_label), msg.c_str());
            gui->show_error(msg);
        }
    }
}

void Op::on_read_out(GObject *src, GAsyncResult *res, gpointer data) {
    Op *op = static_cast<Op *>(data);
    gsize len = 0;
    GError *err = nullptr;
    char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, &err);
    if (line) {
        if (op->kind == OpKind::Check)
            op->check_out += line;
        else if (op->kind == OpKind::Install)
            op->handle_progress_line(line);
        g_free(line);
        g_data_input_stream_read_line_async(op->out, G_PRIORITY_DEFAULT, nullptr, &Op::on_read_out, op);
    } else {
        g_clear_error(&err);
        op->part_done();
    }
}

void Op::on_read_err(GObject *src, GAsyncResult *res, gpointer data) {
    Op *op = static_cast<Op *>(data);
    gsize len = 0;
    GError *err = nullptr;
    char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, &err);
    if (line) {
        op->gui->log(line);
        g_free(line);
        g_data_input_stream_read_line_async(op->err, G_PRIORITY_DEFAULT, nullptr, &Op::on_read_err, op);
    } else {
        g_clear_error(&err);
        op->part_done();
    }
}

void Op::on_wait(GObject *src, GAsyncResult *res, gpointer data) {
    Op *op = static_cast<Op *>(data);
    GError *err = nullptr;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, &err);
    if (err) {
        op->gui->log(err->message);
        g_clear_error(&err);
    } else if (!g_subprocess_get_successful(op->sub)) {
        if (op->kind == OpKind::Install && !op->saw_result) {
            op->gui->log("error: installer exited with code " + std::to_string(g_subprocess_get_exit_status(op->sub)));
        } else if (op->kind == OpKind::Play) {
            op->gui->log("Game exited with code " + std::to_string(g_subprocess_get_exit_status(op->sub)) + ".");
        }
    }
    op->part_done();
}

void Op::part_done() {
    if (++done_parts == 3)
        finish();
}

void Op::finish() {
    Gui *gui = this->gui;
    OpKind kind = this->kind;
    if (kind == OpKind::Check) {
        gui->parse_check_output(check_out);
    } else if (kind == OpKind::Install && !saw_result) {
        gui->log("error: installer exited without a result line");
        gui->show_error("Install failed: the installer exited without reporting a result. See the log.");
    } else if (kind == OpKind::Play) {
        gtk_label_set_text(GTK_LABEL(gui->stage_label), "Game closed.");
        gui->log("Game closed.");
    }
    gui->set_busy(false);
    delete this;
    // An install or a play session may have changed what is on disk, so
    // re-read the product list instead of showing stale status.
    if (kind != OpKind::Check)
        gui->run_check();
}

// --- Gui -----------------------------------------------------------------

std::string Gui::find_setup() {
    static std::string cached;
    if (cached.empty()) {
        char *probe = g_find_program_in_path("wiicompiled-setup");
        cached = probe ? probe : "/app/bin/wiicompiled-setup";
        g_free(probe);
    }
    return cached;
}

void Gui::log(const std::string &line) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(log_buf, &end);
    gtk_text_buffer_insert(log_buf, &end, line.data(), line.size());
    gtk_text_buffer_insert(log_buf, &end, "\n", -1);
    GtkWidget *parent = gtk_widget_get_parent(log_view);
    if (GTK_IS_SCROLLED_WINDOW(parent)) {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent));
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
    }
}

void Gui::show_error(const std::string &msg) {
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(window), GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", msg.c_str());
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

void Gui::refresh_status_row() {
    const char *text;
    bool can_play = false;
    switch (base_status) {
    case GameStatus::Current:
        text = "<b>Base game</b> \xe2\x80\x94 installed, ready to play";
        can_play = true;
        break;
    case GameStatus::Stale:
        text = "<b>Base game</b> \xe2\x80\x94 installed, game assets changed (recompile recommended)";
        can_play = true;
        break;
    case GameStatus::Missing:
        text = "<b>Base game</b> \xe2\x80\x94 not installed";
        break;
    default:
        text = "<b>Base game</b> \xe2\x80\x94 status unknown";
        break;
    }
    gtk_label_set_markup(GTK_LABEL(status_label), text);
    gtk_widget_set_sensitive(play_btn, can_play);
}

void Gui::set_busy(bool busy) {
    const bool can_play = base_status == GameStatus::Current || base_status == GameStatus::Stale;
    gtk_widget_set_sensitive(play_btn, busy ? false : can_play);
    gtk_widget_set_sensitive(install_btn, !busy);
}

void Gui::parse_check_output(const std::string &output) {
    base_status = GameStatus::Missing;
    std::string line;
    std::istringstream iss(output);
    while (std::getline(iss, line)) {
        trim(line);
        if (line.empty() || line.rfind("Nothing installed.", 0) == 0)
            continue;
        const size_t sp = line.find(' ');
        if (sp == std::string::npos)
            continue;
        if (line.substr(0, sp) != "base")
            continue;
        std::string rest = line.substr(sp + 1);
        trim(rest);
        if (rest.rfind("current", 0) == 0)
            base_status = GameStatus::Current;
        else if (rest.rfind("STALE", 0) == 0)
            base_status = GameStatus::Stale;
        else if (rest.rfind("MISSING", 0) == 0)
            base_status = GameStatus::Missing;
    }
    refresh_status_row();
}

bool Gui::spawn(OpKind kind, std::vector<std::string> args) {
    std::vector<const char *> argv;
    argv.reserve(args.size() + 1);
    for (const std::string &a : args)
        argv.push_back(a.c_str());
    argv.push_back(nullptr);

    GError *err = nullptr;
    Op *op = new Op(this, kind);
    op->sub = g_subprocess_newv(
        argv.data(),
        static_cast<GSubprocessFlags>(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE |
                                      G_SUBPROCESS_FLAGS_STDIN_PIPE),
        &err);
    if (!op->sub) {
        log(err ? err->message : "error: failed to spawn wiicompiled-setup");
        g_clear_error(&err);
        delete op;
        return false;
    }
    GOutputStream *stdin_pipe = g_subprocess_get_stdin_pipe(op->sub);
    if (stdin_pipe)
        g_output_stream_close(stdin_pipe, nullptr, nullptr);
    op->out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(op->sub));
    op->err = g_data_input_stream_new(g_subprocess_get_stderr_pipe(op->sub));
    g_data_input_stream_read_line_async(op->out, G_PRIORITY_DEFAULT, nullptr, &Op::on_read_out, op);
    g_data_input_stream_read_line_async(op->err, G_PRIORITY_DEFAULT, nullptr, &Op::on_read_err, op);
    g_subprocess_wait_async(op->sub, nullptr, &Op::on_wait, op);
    return true;
}

void Gui::run_check() {
    if (!spawn(OpKind::Check, {find_setup(), "check-products"}))
        refresh_status_row();
}

void Gui::on_play(GtkButton *btn, gpointer data) {
    Gui *self = static_cast<Gui *>(data);
    (void)btn;
    self->set_busy(true);
    self->log("Launching base game\u2026");
    gtk_label_set_text(GTK_LABEL(self->stage_label), "Launching base game\u2026");
    if (!self->spawn(OpKind::Play, {find_setup(), "launch-base"})) {
        self->set_busy(false);
        self->show_error("Could not launch the game: failed to spawn wiicompiled-setup.");
    }
}

void Gui::on_install(GtkButton *btn, gpointer data) {
    Gui *self = static_cast<Gui *>(data);
    (void)btn;
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new(
        "Select Mario Kart Wii dump", GTK_WINDOW(self->window), GTK_FILE_CHOOSER_ACTION_OPEN, "_Install", "_Cancel");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Disc images");
    for (const char *pattern : {"*.rvz", "*.iso", "*.wbfs", "*.ciso", "*.gcm"})
        gtk_file_filter_add_pattern(filter, pattern);
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), filter);
    GtkFileFilter *all = gtk_file_filter_new();
    gtk_file_filter_set_name(all, "All files");
    gtk_file_filter_add_pattern(all, "*");
    gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(chooser), all);

    if (gtk_native_dialog_run(GTK_NATIVE_DIALOG(chooser)) != GTK_RESPONSE_ACCEPT) {
        g_object_unref(chooser);
        return;
    }
    char *iso = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(chooser));
    g_object_unref(chooser);
    if (!iso)
        return;

    self->set_busy(true);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(self->progress), 0.0);
    self->log("Installing from " + std::string(iso));
    gtk_label_set_text(GTK_LABEL(self->stage_label), "Starting install\u2026");
    if (!self->spawn(OpKind::Install, {find_setup(), "install", "--game", iso, "--progress-json"})) {
        self->set_busy(false);
        self->show_error("Could not start the install: failed to spawn wiicompiled-setup.");
    }
    g_free(iso);
}

void Gui::build_controls_row() {
    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(root), row, FALSE, FALSE, 0);

    status_label = gtk_label_new(nullptr);
    gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
    gtk_box_pack_start(GTK_BOX(row), status_label, TRUE, TRUE, 0);

    install_btn = gtk_button_new_with_label("Install / Update\u2026");
    g_signal_connect(install_btn, "clicked", G_CALLBACK(on_install), this);
    gtk_box_pack_end(GTK_BOX(row), install_btn, FALSE, FALSE, 0);

    play_btn = gtk_button_new_with_label("Play");
    g_signal_connect(play_btn, "clicked", G_CALLBACK(on_play), this);
    gtk_widget_set_sensitive(play_btn, FALSE);
    gtk_box_pack_end(GTK_BOX(row), play_btn, FALSE, FALSE, 0);
}

void Gui::build_status_row() {
    progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(progress), TRUE);
    gtk_box_pack_start(GTK_BOX(root), progress, FALSE, FALSE, 0);

    stage_label = gtk_label_new("Ready.");
    gtk_label_set_xalign(GTK_LABEL(stage_label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(stage_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(root), stage_label, FALSE, FALSE, 0);
}

void Gui::build_log_pane() {
    GtkWidget *scroll = gtk_scrolled_window_new(nullptr, nullptr);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);

    log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(log_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(log_view), GTK_WRAP_WORD_CHAR);

    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "textview { font-family: monospace; }", -1, nullptr);
    gtk_style_context_add_provider(gtk_widget_get_style_context(log_view), GTK_STYLE_PROVIDER(css),
                                   GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);

    log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(log_view));
    gtk_container_add(GTK_CONTAINER(scroll), log_view);
}

void Gui::build_ui() {
    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "WiiCompiled");
    gtk_window_set_default_size(GTK_WINDOW(window), 640, 480);

    root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(root), 12);
    gtk_container_add(GTK_CONTAINER(window), root);

    build_controls_row();
    build_status_row();
    build_log_pane();

    refresh_status_row();
    gtk_widget_show_all(window);
    log("WiiCompiled launcher ready.");
    run_check();
}

void Gui::activate(GtkApplication *app, gpointer data) {
    (void)data;
    new Gui(app);
}

}  // namespace

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(&Gui::activate), nullptr);
    const int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
