/* WiiCompiled launcher: a minimal GTK3 frontend for the Flatpak bundle.
 *
 * It never compiles anything itself. It shells out to the bundled
 * wiicompiled-setup entrypoint (which owns the workspace sync and the
 * toolchain environment) and renders its machine-readable output:
 *   - check-products        -> the base-game status row
 *   - install --game ISO --progress-json -> progress bar + log pane
 *   - launch-base           -> play the installed game
 */
#include <gtk/gtk.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_ID "io.github.TeamWheelWizard.Wiicompiled"

typedef enum { ST_UNKNOWN, ST_MISSING, ST_CURRENT, ST_STALE } GameStatus;

typedef struct {
    GtkApplication *app;
    GtkWidget *window;
    GtkWidget *status_label;
    GtkWidget *stage_label;
    GtkWidget *progress;
    GtkWidget *log_view;
    GtkTextBuffer *log_buf;
    GtkWidget *play_btn;
    GtkWidget *install_btn;
    GameStatus base_status;
} Gui;

typedef enum { OP_CHECK, OP_INSTALL, OP_PLAY } OpKind;

typedef struct {
    Gui *gui;
    GSubprocess *sub;
    GDataInputStream *out;
    GDataInputStream *err;
    GString *check_out;
    OpKind kind;
    gboolean saw_result;
    gboolean busy_logged;
    int done_parts;
} Op;

static const char *find_setup(void) {
    static char *cached = NULL;
    if (!cached) {
        cached = g_find_program_in_path("wiicompiled-setup");
        if (!cached)
            cached = g_strdup("/app/bin/wiicompiled-setup");
    }
    return cached;
}

static void log_line(Gui *gui, const char *line) {
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(gui->log_buf, &end);
    gtk_text_buffer_insert(gui->log_buf, &end, line, -1);
    gtk_text_buffer_insert(gui->log_buf, &end, "\n", -1);
    GtkWidget *parent = gtk_widget_get_parent(gui->log_view);
    if (GTK_IS_SCROLLED_WINDOW(parent)) {
        GtkAdjustment *adj = gtk_scrolled_window_get_vadjustment(GTK_SCROLLED_WINDOW(parent));
        gtk_adjustment_set_value(adj, gtk_adjustment_get_upper(adj) - gtk_adjustment_get_page_size(adj));
    }
}

static void show_error(Gui *gui, const char *msg) {
    GtkWidget *dlg = gtk_message_dialog_new(GTK_WINDOW(gui->window), GTK_DIALOG_MODAL,
                                            GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE, "%s", msg);
    gtk_dialog_run(GTK_DIALOG(dlg));
    gtk_widget_destroy(dlg);
}

/* Minimal JSON value extraction for the flat --progress-json lines. */
static char *json_string(const char *line, const char *key) {
    char *pat = g_strdup_printf("\"%s\":\"", key);
    const char *p = strstr(line, pat);
    g_free(pat);
    if (!p)
        return NULL;
    p += strlen(key) + 4;
    GString *s = g_string_new(NULL);
    for (; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': g_string_append_c(s, '\n'); break;
            case 't': g_string_append_c(s, '\t'); break;
            case 'r': g_string_append_c(s, '\r'); break;
            case 'u':
                if (p[1] && p[2] && p[3] && p[4]) {
                    char hex[5] = { p[1], p[2], p[3], p[4], 0 };
                    gunichar c = (gunichar)strtoul(hex, NULL, 16);
                    char utf8[7] = { 0 };
                    g_unichar_to_utf8(c, utf8);
                    g_string_append(s, utf8);
                    p += 4;
                }
                break;
            default: g_string_append_c(s, *p); break;
            }
        } else {
            g_string_append_c(s, *p);
        }
    }
    return g_string_free(s, FALSE);
}

static long json_int(const char *line, const char *key) {
    char *pat = g_strdup_printf("\"%s\":", key);
    const char *p = strstr(line, pat);
    g_free(pat);
    return p ? strtol(p + strlen(key) + 3, NULL, 10) : -1;
}

static gboolean json_is_true(const char *line, const char *key) {
    char *pat = g_strdup_printf("\"%s\":true", key);
    gboolean found = strstr(line, pat) != NULL;
    g_free(pat);
    return found;
}

static void refresh_status_row(Gui *gui) {
    const char *text;
    gboolean can_play = FALSE;
    switch (gui->base_status) {
    case ST_CURRENT:
        text = "<b>Base game</b> \xe2\x80\x94 installed, ready to play";
        can_play = TRUE;
        break;
    case ST_STALE:
        text = "<b>Base game</b> \xe2\x80\x94 installed, game assets changed (recompile recommended)";
        can_play = TRUE;
        break;
    case ST_MISSING:
        text = "<b>Base game</b> \xe2\x80\x94 not installed";
        break;
    default:
        text = "<b>Base game</b> \xe2\x80\x94 status unknown";
        break;
    }
    gtk_label_set_markup(GTK_LABEL(gui->status_label), text);
    gtk_widget_set_sensitive(gui->play_btn, can_play);
}

static void set_busy(Gui *gui, gboolean busy) {
    gtk_widget_set_sensitive(gui->play_btn, busy ? FALSE : (gui->base_status == ST_CURRENT || gui->base_status == ST_STALE));
    gtk_widget_set_sensitive(gui->install_btn, !busy);
}

static Op *op_new(Gui *gui, OpKind kind) {
    Op *op = g_new0(Op, 1);
    op->gui = gui;
    op->kind = kind;
    op->check_out = g_string_new(NULL);
    return op;
}

static void parse_check_output(Gui *gui, const char *output) {
    gui->base_status = ST_MISSING;
    char **lines = g_strsplit(output, "\n", -1);
    for (int i = 0; lines[i]; i++) {
        char *line = g_strstrip(lines[i]);
        if (!line[0] || g_str_has_prefix(line, "Nothing installed."))
            continue;
        char *sp = strchr(line, ' ');
        if (!sp)
            continue;
        *sp = 0;
        if (strcmp(line, "base") != 0)
            continue;
        char *rest = g_strchug(sp + 1);
        if (g_str_has_prefix(rest, "current"))
            gui->base_status = ST_CURRENT;
        else if (g_str_has_prefix(rest, "STALE"))
            gui->base_status = ST_STALE;
        else if (g_str_has_prefix(rest, "MISSING"))
            gui->base_status = ST_MISSING;
    }
    g_strfreev(lines);
    refresh_status_row(gui);
}

static void run_check(Gui *gui);

static void op_finish(Op *op) {
    Gui *gui = op->gui;
    OpKind kind = op->kind;
    if (kind == OP_CHECK) {
        parse_check_output(gui, op->check_out->str);
    } else if (kind == OP_INSTALL && !op->saw_result) {
        log_line(gui, "error: installer exited without a result line");
        show_error(gui, "Install failed: the installer exited without reporting a result. See the log.");
    } else if (kind == OP_PLAY) {
        gtk_label_set_text(GTK_LABEL(gui->stage_label), "Game closed.");
        log_line(gui, "Game closed.");
    }
    set_busy(gui, FALSE);
    g_clear_object(&op->out);
    g_clear_object(&op->err);
    g_clear_object(&op->sub);
    g_string_free(op->check_out, TRUE);
    g_free(op);
    /* An install or a play session may have changed what is on disk, so
     * re-read the product list instead of showing stale status. */
    if (kind != OP_CHECK)
        run_check(gui);
}

static void op_part_done(Op *op) {
    if (++op->done_parts == 3)
        op_finish(op);
}

static void handle_progress_line(Op *op, const char *line) {
    Gui *gui = op->gui;
    if (strstr(line, "\"type\":\"progress\"")) {
        long pct = json_int(line, "percent");
        char *msg = json_string(line, "message");
        if (pct >= 0 && pct <= 100)
            gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(gui->progress), pct / 100.0);
        gtk_label_set_text(GTK_LABEL(gui->stage_label), msg ? msg : "");
        g_free(msg);
    } else if (strstr(line, "\"type\":\"result\"")) {
        op->saw_result = TRUE;
        gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(gui->progress), 1.0);
        if (json_is_true(line, "success")) {
            char *dir = json_string(line, "installDir");
            GString *msg = g_string_new("Install complete");
            if (dir && dir[0])
                g_string_append_printf(msg, ": %s", dir);
            log_line(gui, msg->str);
            gtk_label_set_text(GTK_LABEL(gui->stage_label), msg->str);
            g_string_free(msg, TRUE);
            g_free(dir);
        } else {
            char *err = json_string(line, "error");
            GString *msg = g_string_new("Install failed: ");
            g_string_append(msg, err && err[0] ? err : "unknown error");
            log_line(gui, msg->str);
            gtk_label_set_text(GTK_LABEL(gui->stage_label), msg->str);
            show_error(gui, msg->str);
            g_string_free(msg, TRUE);
            g_free(err);
        }
    }
}

static void read_out_cb(GObject *src, GAsyncResult *res, gpointer data) {
    Op *op = data;
    gsize len = 0;
    GError *err = NULL;
    char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, &err);
    if (line) {
        if (op->kind == OP_CHECK)
            g_string_append_printf(op->check_out, "%s\n", line);
        else if (op->kind == OP_INSTALL)
            handle_progress_line(op, line);
        g_free(line);
        g_data_input_stream_read_line_async(op->out, G_PRIORITY_DEFAULT, NULL, read_out_cb, op);
    } else {
        g_clear_error(&err);
        op_part_done(op);
    }
}

static void read_err_cb(GObject *src, GAsyncResult *res, gpointer data) {
    Op *op = data;
    gsize len = 0;
    GError *err = NULL;
    char *line = g_data_input_stream_read_line_finish(G_DATA_INPUT_STREAM(src), res, &len, &err);
    if (line) {
        log_line(op->gui, line);
        g_free(line);
        g_data_input_stream_read_line_async(op->err, G_PRIORITY_DEFAULT, NULL, read_err_cb, op);
    } else {
        g_clear_error(&err);
        op_part_done(op);
    }
}

static void wait_cb(GObject *src, GAsyncResult *res, gpointer data) {
    Op *op = data;
    GError *err = NULL;
    g_subprocess_wait_finish(G_SUBPROCESS(src), res, &err);
    if (err) {
        log_line(op->gui, err->message);
        g_clear_error(&err);
    } else if (!g_subprocess_get_successful(op->sub)) {
        if (op->kind == OP_INSTALL && !op->saw_result) {
            char *msg = g_strdup_printf("error: installer exited with code %d", g_subprocess_get_exit_status(op->sub));
            log_line(op->gui, msg);
            g_free(msg);
        } else if (op->kind == OP_PLAY) {
            char *msg = g_strdup_printf("Game exited with code %d.", g_subprocess_get_exit_status(op->sub));
            log_line(op->gui, msg);
            g_free(msg);
        }
    }
    op_part_done(op);
}

static gboolean spawn_setup(Gui *gui, OpKind kind, char **argv) {
    GError *err = NULL;
    Op *op = op_new(gui, kind);
    op->sub = g_subprocess_newv((const gchar *const *)argv,
                                G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                                G_SUBPROCESS_FLAGS_STDERR_PIPE |
                                G_SUBPROCESS_FLAGS_STDIN_PIPE, &err);
    if (!op->sub) {
        log_line(gui, err ? err->message : "error: failed to spawn wiicompiled-setup");
        g_clear_error(&err);
        g_string_free(op->check_out, TRUE);
        g_free(op);
        return FALSE;
    }
    GOutputStream *stdin_pipe = g_subprocess_get_stdin_pipe(op->sub);
    if (stdin_pipe)
        g_output_stream_close(stdin_pipe, NULL, NULL);
    op->out = g_data_input_stream_new(g_subprocess_get_stdout_pipe(op->sub));
    op->err = g_data_input_stream_new(g_subprocess_get_stderr_pipe(op->sub));
    g_data_input_stream_read_line_async(op->out, G_PRIORITY_DEFAULT, NULL, read_out_cb, op);
    g_data_input_stream_read_line_async(op->err, G_PRIORITY_DEFAULT, NULL, read_err_cb, op);
    g_subprocess_wait_async(op->sub, NULL, wait_cb, op);
    return TRUE;
}

static void run_check(Gui *gui) {
    const char *setup = find_setup();
    char *argv[] = { (char *)setup, "check-products", NULL };
    if (!spawn_setup(gui, OP_CHECK, argv))
        refresh_status_row(gui);
}

static void on_play(GtkButton *btn, gpointer data) {
    Gui *gui = data;
    (void)btn;
    set_busy(gui, TRUE);
    gtk_label_set_text(GTK_LABEL(gui->stage_label), "Launching base game\u2026");
    log_line(gui, "Launching base game\u2026");
    const char *setup = find_setup();
    char *argv[] = { (char *)setup, "launch-base", NULL };
    if (!spawn_setup(gui, OP_PLAY, argv)) {
        set_busy(gui, FALSE);
        show_error(gui, "Could not launch the game: failed to spawn wiicompiled-setup.");
    }
}

static void on_install(GtkButton *btn, gpointer data) {
    Gui *gui = data;
    (void)btn;
    GtkFileChooserNative *chooser = gtk_file_chooser_native_new("Select Mario Kart Wii dump", GTK_WINDOW(gui->window),
                                                     GTK_FILE_CHOOSER_ACTION_OPEN, "_Install", "_Cancel");
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "Disc images");
    gtk_file_filter_add_pattern(filter, "*.rvz");
    gtk_file_filter_add_pattern(filter, "*.iso");
    gtk_file_filter_add_pattern(filter, "*.wbfs");
    gtk_file_filter_add_pattern(filter, "*.ciso");
    gtk_file_filter_add_pattern(filter, "*.gcm");
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
    set_busy(gui, TRUE);
    gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(gui->progress), 0.0);
    gtk_label_set_text(GTK_LABEL(gui->stage_label), "Starting install\u2026");
    char *msg = g_strdup_printf("Installing from %s", iso);
    log_line(gui, msg);
    g_free(msg);
    const char *setup = find_setup();
    char *argv[] = { (char *)setup, "install", "--game", iso, "--progress-json", NULL };
    if (!spawn_setup(gui, OP_INSTALL, argv)) {
        set_busy(gui, FALSE);
        show_error(gui, "Could not start the install: failed to spawn wiicompiled-setup.");
    }
    g_free(iso);
}

static void activate(GtkApplication *app, gpointer data) {
    Gui *gui = g_new0(Gui, 1);
    gui->app = app;
    gui->base_status = ST_UNKNOWN;

    gui->window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(gui->window), "WiiCompiled");
    gtk_window_set_default_size(GTK_WINDOW(gui->window), 640, 480);

    GtkWidget *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_container_set_border_width(GTK_CONTAINER(root), 12);
    gtk_container_add(GTK_CONTAINER(gui->window), root);

    GtkWidget *row = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_pack_start(GTK_BOX(root), row, FALSE, FALSE, 0);
    gui->status_label = gtk_label_new(NULL);
    gtk_label_set_xalign(GTK_LABEL(gui->status_label), 0.0);
    gtk_box_pack_start(GTK_BOX(row), gui->status_label, TRUE, TRUE, 0);
    gui->install_btn = gtk_button_new_with_label("Install / Update\u2026");
    g_signal_connect(gui->install_btn, "clicked", G_CALLBACK(on_install), gui);
    gtk_box_pack_end(GTK_BOX(row), gui->install_btn, FALSE, FALSE, 0);
    gui->play_btn = gtk_button_new_with_label("Play");
    g_signal_connect(gui->play_btn, "clicked", G_CALLBACK(on_play), gui);
    gtk_widget_set_sensitive(gui->play_btn, FALSE);
    gtk_box_pack_end(GTK_BOX(row), gui->play_btn, FALSE, FALSE, 0);

    gui->progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(gui->progress), TRUE);
    gtk_box_pack_start(GTK_BOX(root), gui->progress, FALSE, FALSE, 0);
    gui->stage_label = gtk_label_new("Ready.");
    gtk_label_set_xalign(GTK_LABEL(gui->stage_label), 0.0);
    gtk_label_set_ellipsize(GTK_LABEL(gui->stage_label), PANGO_ELLIPSIZE_MIDDLE);
    gtk_box_pack_start(GTK_BOX(root), gui->stage_label, FALSE, FALSE, 0);

    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_pack_start(GTK_BOX(root), scroll, TRUE, TRUE, 0);
    gui->log_view = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(gui->log_view), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(gui->log_view), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(gui->log_view), GTK_WRAP_WORD_CHAR);
    GtkCssProvider *css = gtk_css_provider_new();
    gtk_css_provider_load_from_data(css, "textview { font-family: monospace; }", -1, NULL);
    gtk_style_context_add_provider(gtk_widget_get_style_context(gui->log_view),
                                   GTK_STYLE_PROVIDER(css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(css);
    gui->log_buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(gui->log_view));
    gtk_container_add(GTK_CONTAINER(scroll), gui->log_view);

    (void)data;
    refresh_status_row(gui);
    gtk_widget_show_all(gui->window);
    log_line(gui, "WiiCompiled launcher ready.");
    run_check(gui);
}

int main(int argc, char **argv) {
    GtkApplication *app = gtk_application_new(APP_ID, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    int status = g_application_run(G_APPLICATION(app), argc, argv);
    g_object_unref(app);
    return status;
}
