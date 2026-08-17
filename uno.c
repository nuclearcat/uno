#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define UNO_VERSION "0.1"
#define CTRL_KEY(k) ((k) & 0x1f)
#define TAB_STOP 8

enum editor_key {
    KEY_BACKSPACE = 127,
    KEY_ARROW_LEFT = 1000,
    KEY_ARROW_RIGHT,
    KEY_ARROW_UP,
    KEY_ARROW_DOWN,
    KEY_DELETE,
    KEY_HOME,
    KEY_END,
    KEY_PAGE_UP,
    KEY_PAGE_DOWN,
    KEY_F3
};

typedef struct {
    size_t size;
    char *chars;
} editor_row;

typedef struct {
    int cx;
    int cy;
    int rowoff;
    int coloff;
    int screenrows;
    int screencols;
    editor_row *rows;
    int numrows;
    int dirty;
    char *filename;
    char *clipboard;
    size_t clipboard_len;
    char *last_search;
    char status[256];
    time_t status_time;
    struct termios original_termios;
    bool raw_enabled;
    bool quit;
} editor_state;

static editor_state E;

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} append_buffer;

static void editor_refresh_screen(void);
static void editor_set_status(const char *fmt, ...);

static void terminal_write_best_effort(const char *s, size_t len)
{
    ssize_t ignored = write(STDOUT_FILENO, s, len);
    (void)ignored;
}

static void die(const char *message)
{
    if (E.raw_enabled)
        terminal_write_best_effort("\x1b[2J\x1b[H", 7);
    perror(message);
    exit(EXIT_FAILURE);
}

static void disable_raw_mode(void)
{
    if (E.raw_enabled) {
        (void)tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.original_termios);
        E.raw_enabled = false;
    }
}

static void enable_raw_mode(void)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        fprintf(stderr, "uno: standard input and output must be a terminal\n");
        exit(EXIT_FAILURE);
    }
    if (tcgetattr(STDIN_FILENO, &E.original_termios) == -1)
        die("tcgetattr");
    if (atexit(disable_raw_mode) != 0)
        die("atexit");

    raw = E.original_termios;
    raw.c_iflag &= (tcflag_t)~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= (tcflag_t)~OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
    E.raw_enabled = true;
    terminal_write_best_effort("\x1b[?25l", 6);
}

static int read_byte(char *out)
{
    ssize_t n;

    do {
        n = read(STDIN_FILENO, out, 1);
    } while (n == -1 && errno == EINTR);
    if (n == -1)
        die("read");
    return n == 1;
}

static int editor_read_key(void)
{
    char c;

    while (!read_byte(&c))
        ;
    if (c != '\x1b')
        return (unsigned char)c;

    char seq[8] = {0};
    if (!read_byte(&seq[0]))
        return '\x1b';
    if (!read_byte(&seq[1]))
        return '\x1b';

    if (seq[0] == '[') {
        if (seq[1] >= '0' && seq[1] <= '9') {
            size_t i = 2;
            while (i < sizeof(seq) - 1 && read_byte(&seq[i])) {
                if (seq[i] == '~')
                    break;
                i++;
            }
            if (strcmp(seq + 1, "1~") == 0 || strcmp(seq + 1, "7~") == 0)
                return KEY_HOME;
            if (strcmp(seq + 1, "3~") == 0)
                return KEY_DELETE;
            if (strcmp(seq + 1, "4~") == 0 || strcmp(seq + 1, "8~") == 0)
                return KEY_END;
            if (strcmp(seq + 1, "5~") == 0)
                return KEY_PAGE_UP;
            if (strcmp(seq + 1, "6~") == 0)
                return KEY_PAGE_DOWN;
            if (strcmp(seq + 1, "13~") == 0)
                return KEY_F3;
        } else {
            switch (seq[1]) {
            case 'A': return KEY_ARROW_UP;
            case 'B': return KEY_ARROW_DOWN;
            case 'C': return KEY_ARROW_RIGHT;
            case 'D': return KEY_ARROW_LEFT;
            case 'H': return KEY_HOME;
            case 'F': return KEY_END;
            }
        }
    } else if (seq[0] == 'O') {
        switch (seq[1]) {
        case 'H': return KEY_HOME;
        case 'F': return KEY_END;
        case 'R': return KEY_F3;
        }
    }
    return '\x1b';
}

static int get_window_size(int *rows, int *cols)
{
    struct winsize ws;

    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0)
        return -1;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

static void ab_append(append_buffer *ab, const char *s, size_t len)
{
    if (len == 0)
        return;
    if (ab->len + len > ab->cap) {
        size_t new_cap = ab->cap ? ab->cap : 1024;
        while (new_cap < ab->len + len)
            new_cap *= 2;
        char *new_data = realloc(ab->data, new_cap);
        if (!new_data)
            die("realloc");
        ab->data = new_data;
        ab->cap = new_cap;
    }
    memcpy(ab->data + ab->len, s, len);
    ab->len += len;
}

static void row_insert_char(editor_row *row, size_t at, int c)
{
    if (at > row->size)
        at = row->size;
    char *p = realloc(row->chars, row->size + 2);
    if (!p)
        die("realloc");
    row->chars = p;
    memmove(row->chars + at + 1, row->chars + at, row->size - at + 1);
    row->chars[at] = (char)c;
    row->size++;
}

static void row_append_string(editor_row *row, const char *s, size_t len)
{
    char *p = realloc(row->chars, row->size + len + 1);
    if (!p)
        die("realloc");
    row->chars = p;
    memcpy(row->chars + row->size, s, len);
    row->size += len;
    row->chars[row->size] = '\0';
}

static void row_delete_char(editor_row *row, size_t at)
{
    if (at >= row->size)
        return;
    memmove(row->chars + at, row->chars + at + 1, row->size - at);
    row->size--;
}

static void editor_insert_row(int at, const char *s, size_t len)
{
    if (at < 0 || at > E.numrows)
        return;
    editor_row *p = realloc(E.rows, sizeof(*E.rows) * (size_t)(E.numrows + 1));
    if (!p)
        die("realloc");
    E.rows = p;
    memmove(E.rows + at + 1, E.rows + at,
            sizeof(*E.rows) * (size_t)(E.numrows - at));
    E.rows[at].chars = malloc(len + 1);
    if (!E.rows[at].chars)
        die("malloc");
    memcpy(E.rows[at].chars, s, len);
    E.rows[at].chars[len] = '\0';
    E.rows[at].size = len;
    E.numrows++;
}

static void editor_delete_row(int at)
{
    if (at < 0 || at >= E.numrows)
        return;
    free(E.rows[at].chars);
    memmove(E.rows + at, E.rows + at + 1,
            sizeof(*E.rows) * (size_t)(E.numrows - at - 1));
    E.numrows--;
}

static void editor_insert_char(int c)
{
    row_insert_char(&E.rows[E.cy], (size_t)E.cx, c);
    E.cx++;
    E.dirty++;
}

static void editor_insert_newline(void)
{
    editor_row *row = &E.rows[E.cy];
    editor_insert_row(E.cy + 1, row->chars + E.cx, row->size - (size_t)E.cx);
    row = &E.rows[E.cy];
    row->size = (size_t)E.cx;
    row->chars[row->size] = '\0';
    E.cy++;
    E.cx = 0;
    E.dirty++;
}

static void editor_delete_char(void)
{
    editor_row *row = &E.rows[E.cy];

    if (E.cx > 0) {
        row_delete_char(row, (size_t)E.cx - 1);
        E.cx--;
        E.dirty++;
    } else if (E.cy > 0) {
        int previous_len = (int)E.rows[E.cy - 1].size;
        row_append_string(&E.rows[E.cy - 1], row->chars, row->size);
        editor_delete_row(E.cy);
        E.cy--;
        E.cx = previous_len;
        E.dirty++;
    }
}

static void editor_delete_forward(void)
{
    editor_row *row = &E.rows[E.cy];

    if ((size_t)E.cx < row->size) {
        row_delete_char(row, (size_t)E.cx);
        E.dirty++;
    } else if (E.cy + 1 < E.numrows) {
        row_append_string(row, E.rows[E.cy + 1].chars, E.rows[E.cy + 1].size);
        editor_delete_row(E.cy + 1);
        E.dirty++;
    }
}

static void editor_cut_line(void)
{
    editor_row *row = &E.rows[E.cy];
    bool has_next = E.cy + 1 < E.numrows;
    bool has_previous = E.cy > 0;
    size_t prefix = (!has_next && has_previous) ? 1 : 0;
    size_t suffix = has_next ? 1 : 0;

    free(E.clipboard);
    E.clipboard_len = prefix + row->size + suffix;
    E.clipboard = malloc(E.clipboard_len + 1);
    if (!E.clipboard)
        die("malloc");
    size_t pos = 0;
    if (prefix)
        E.clipboard[pos++] = '\n';
    memcpy(E.clipboard + pos, row->chars, row->size);
    pos += row->size;
    if (suffix)
        E.clipboard[pos++] = '\n';
    E.clipboard[pos] = '\0';

    if (E.numrows == 1) {
        row->size = 0;
        row->chars[0] = '\0';
        E.cx = 0;
    } else {
        int old_cy = E.cy;
        editor_delete_row(E.cy);
        if (old_cy >= E.numrows) {
            E.cy = E.numrows - 1;
            E.cx = (int)E.rows[E.cy].size;
        } else {
            E.cx = 0;
        }
    }
    E.dirty++;
    editor_set_status("Cut one line");
}

static void editor_paste(void)
{
    if (!E.clipboard) {
        editor_set_status("Clipboard is empty");
        return;
    }
    for (size_t i = 0; i < E.clipboard_len; i++) {
        if (E.clipboard[i] == '\n')
            editor_insert_newline();
        else
            editor_insert_char((unsigned char)E.clipboard[i]);
    }
    editor_set_status("Pasted cut line");
}

static int editor_row_display_x(const editor_row *row, int cx)
{
    int rx = 0;
    int limit = cx < (int)row->size ? cx : (int)row->size;
    for (int i = 0; i < limit; i++) {
        if (row->chars[i] == '\t')
            rx += TAB_STOP - (rx % TAB_STOP);
        else
            rx++;
    }
    return rx;
}

static void editor_scroll(void)
{
    int rx = editor_row_display_x(&E.rows[E.cy], E.cx);

    if (E.cy < E.rowoff)
        E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows)
        E.rowoff = E.cy - E.screenrows + 1;
    if (rx < E.coloff)
        E.coloff = rx;
    if (rx >= E.coloff + E.screencols)
        E.coloff = rx - E.screencols + 1;
}

static void draw_text_row(append_buffer *ab, const editor_row *row)
{
    int rx = 0;
    int drawn = 0;

    for (size_t i = 0; i < row->size && drawn < E.screencols; i++) {
        unsigned char c = (unsigned char)row->chars[i];
        if (c == '\t') {
            int spaces = TAB_STOP - (rx % TAB_STOP);
            for (int j = 0; j < spaces; j++, rx++) {
                if (rx >= E.coloff && drawn < E.screencols) {
                    ab_append(ab, " ", 1);
                    drawn++;
                }
            }
        } else {
            if (rx >= E.coloff) {
                if (c < 32 || c == 127)
                    ab_append(ab, "?", 1);
                else
                    ab_append(ab, (const char *)&row->chars[i], 1);
                drawn++;
            }
            rx++;
        }
    }
}

static void editor_draw_rows(append_buffer *ab)
{
    for (int y = 0; y < E.screenrows; y++) {
        int file_row = y + E.rowoff;
        if (file_row >= E.numrows) {
            if (E.numrows == 1 && E.rows[0].size == 0 && y == E.screenrows / 3) {
                char welcome[80];
                int len = snprintf(welcome, sizeof(welcome),
                                   "uno %s - a tiny terminal editor", UNO_VERSION);
                int padding = (E.screencols - len) / 2;
                if (padding > 0) {
                    ab_append(ab, "~", 1);
                    padding--;
                }
                while (padding-- > 0)
                    ab_append(ab, " ", 1);
                if (len > E.screencols)
                    len = E.screencols;
                ab_append(ab, welcome, (size_t)len);
            } else {
                ab_append(ab, "~", 1);
            }
        } else {
            draw_text_row(ab, &E.rows[file_row]);
        }
        ab_append(ab, "\x1b[K", 3);
        if (y < E.screenrows - 1)
            ab_append(ab, "\r\n", 2);
    }
}

static void editor_draw_status_bar(append_buffer *ab)
{
    char left[160];
    char right[64];
    const char *name = E.filename ? E.filename : "[No Name]";
    int left_len = snprintf(left, sizeof(left), " %.80s - %d line%s %s", name,
                            E.numrows, E.numrows == 1 ? "" : "s",
                            E.dirty ? "(modified)" : "");
    int right_len = snprintf(right, sizeof(right), " %d:%d ", E.cy + 1, E.cx + 1);
    if (left_len < 0)
        left_len = 0;
    if (right_len < 0)
        right_len = 0;
    if (left_len > E.screencols)
        left_len = E.screencols;

    ab_append(ab, "\x1b[7m", 4);
    ab_append(ab, left, (size_t)left_len);
    int used = left_len;
    while (used < E.screencols) {
        if (E.screencols - used == right_len) {
            ab_append(ab, right, (size_t)right_len);
            used += right_len;
        } else {
            ab_append(ab, " ", 1);
            used++;
        }
    }
    ab_append(ab, "\x1b[m", 3);
    ab_append(ab, "\r\n", 2);
}

static void editor_draw_message_bar(append_buffer *ab)
{
    static const char help[] =
        "^O Save  ^X Exit  ^W Find  ^\\ Replace  ^K Cut  ^U Paste  F3 Next";
    const char *message = help;
    size_t len = strlen(help);

    ab_append(ab, "\x1b[K", 3);
    if (E.status[0] && time(NULL) - E.status_time < 5) {
        message = E.status;
        len = strlen(E.status);
    }
    if (len > (size_t)E.screencols)
        len = (size_t)E.screencols;
    ab_append(ab, message, len);
}

static void editor_refresh_screen(void)
{
    int rows, cols;
    if (get_window_size(&rows, &cols) == 0) {
        E.screenrows = rows > 2 ? rows - 2 : 1;
        E.screencols = cols > 0 ? cols : 1;
    }
    editor_scroll();

    append_buffer ab = {0};
    ab_append(&ab, "\x1b[?25l\x1b[H", 9);
    editor_draw_rows(&ab);
    ab_append(&ab, "\r\n", 2);
    editor_draw_status_bar(&ab);
    editor_draw_message_bar(&ab);

    int rx = editor_row_display_x(&E.rows[E.cy], E.cx);
    char cursor[40];
    int len = snprintf(cursor, sizeof(cursor), "\x1b[%d;%dH",
                       E.cy - E.rowoff + 1, rx - E.coloff + 1);
    ab_append(&ab, cursor, (size_t)len);
    ab_append(&ab, "\x1b[?25h", 6);

    size_t written = 0;
    while (written < ab.len) {
        ssize_t n = write(STDOUT_FILENO, ab.data + written, ab.len - written);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            free(ab.data);
            die("write");
        }
        written += (size_t)n;
    }
    free(ab.data);
}

static void editor_set_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(E.status, sizeof(E.status), fmt, ap);
    va_end(ap);
    E.status_time = time(NULL);
}

static char *editor_prompt(const char *prompt, bool allow_empty)
{
    size_t cap = 64;
    size_t len = 0;
    char *buffer = malloc(cap);
    if (!buffer)
        die("malloc");
    buffer[0] = '\0';

    for (;;) {
        editor_set_status(prompt, buffer);
        editor_refresh_screen();
        int c = editor_read_key();
        if (c == '\r' || c == '\n') {
            if (len != 0 || allow_empty) {
                E.status[0] = '\0';
                return buffer;
            }
        } else if (c == '\x1b') {
            E.status[0] = '\0';
            free(buffer);
            return NULL;
        } else if (c == KEY_BACKSPACE || c == CTRL_KEY('h') || c == KEY_DELETE) {
            if (len != 0)
                buffer[--len] = '\0';
        } else if ((c >= 32 && c < 127) || (c >= 128 && c <= 255)) {
            if (len + 1 >= cap) {
                cap *= 2;
                char *p = realloc(buffer, cap);
                if (!p)
                    die("realloc");
                buffer = p;
            }
            buffer[len++] = (char)c;
            buffer[len] = '\0';
        }
    }
}

static bool editor_find_from(const char *query, int start_row, size_t start_col,
                             bool wrap)
{
    int row = start_row;
    size_t col = start_col;

    for (int checked = 0; checked < E.numrows; checked++) {
        editor_row *r = &E.rows[row];
        if (col <= r->size) {
            char *match = strstr(r->chars + col, query);
            if (match) {
                E.cy = row;
                E.cx = (int)(match - r->chars);
                E.rowoff = E.cy;
                return true;
            }
        }
        row++;
        col = 0;
        if (row == E.numrows) {
            if (!wrap)
                break;
            row = 0;
        }
    }
    if (wrap && start_col > 0) {
        editor_row *r = &E.rows[start_row];
        char *match = strstr(r->chars, query);
        if (match && (size_t)(match - r->chars) < start_col) {
            E.cy = start_row;
            E.cx = (int)(match - r->chars);
            E.rowoff = E.cy;
            return true;
        }
    }
    return false;
}

static void editor_search(bool next)
{
    if (!next) {
        char *query = editor_prompt("Search: %s  (Esc cancels)", false);
        if (!query)
            return;
        free(E.last_search);
        E.last_search = query;
    }
    if (!E.last_search) {
        editor_set_status("No previous search");
        return;
    }

    size_t start = next ? (size_t)E.cx + 1 : (size_t)E.cx;
    if (editor_find_from(E.last_search, E.cy, start, true))
        editor_set_status("Found: %s", E.last_search);
    else
        editor_set_status("Not found: %s", E.last_search);
}

static void row_replace(editor_row *row, size_t at, size_t old_len,
                        const char *replacement, size_t new_len)
{
    size_t final_size = row->size - old_len + new_len;
    char *p = malloc(final_size + 1);
    if (!p)
        die("malloc");
    memcpy(p, row->chars, at);
    memcpy(p + at, replacement, new_len);
    memcpy(p + at + new_len, row->chars + at + old_len,
           row->size - at - old_len + 1);
    free(row->chars);
    row->chars = p;
    row->size = final_size;
}

static void editor_replace(void)
{
    char *query = editor_prompt("Search to replace: %s  (Esc cancels)", false);
    if (!query)
        return;
    char *replacement = editor_prompt("Replace with: %s  (Esc cancels)", true);
    if (!replacement) {
        free(query);
        return;
    }

    size_t query_len = strlen(query);
    size_t replacement_len = strlen(replacement);
    int count = 0;
    bool replace_all = false;
    bool stop = false;

    for (int y = 0; y < E.numrows && !stop; y++) {
        size_t at = 0;
        while (at <= E.rows[y].size) {
            char *match = strstr(E.rows[y].chars + at, query);
            if (!match)
                break;
            size_t match_at = (size_t)(match - E.rows[y].chars);
            E.cy = y;
            E.cx = (int)match_at;

            int answer = 'y';
            if (!replace_all) {
                editor_set_status("Replace? y=yes n=no a=all q=quit");
                editor_refresh_screen();
                do {
                    answer = editor_read_key();
                    if (answer >= 0 && answer <= 255)
                        answer = tolower((unsigned char)answer);
                } while (answer != 'y' && answer != 'n' &&
                         answer != 'a' && answer != 'q' && answer != '\x1b');
            }
            if (answer == 'q' || answer == '\x1b') {
                stop = true;
                break;
            }
            if (answer == 'a')
                replace_all = true;
            if (answer == 'y' || answer == 'a') {
                row_replace(&E.rows[y], match_at, query_len,
                            replacement, replacement_len);
                E.dirty++;
                count++;
                at = match_at + replacement_len;
            } else {
                at = match_at + query_len;
            }
        }
    }
    editor_set_status("Replaced %d occurrence%s", count, count == 1 ? "" : "s");
    free(query);
    free(replacement);
}

static int write_all(int fd, const char *buffer, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = write(fd, buffer + done, len - done);
        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static bool editor_save(void)
{
    if (!E.filename) {
        char *name = editor_prompt("File name to save: %s  (Esc cancels)", false);
        if (!name) {
            editor_set_status("Save cancelled");
            return false;
        }
        E.filename = name;
    }

    int fd = open(E.filename, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd == -1) {
        editor_set_status("Cannot save: %s", strerror(errno));
        return false;
    }

    size_t total = 0;
    bool ok = true;
    for (int i = 0; i < E.numrows; i++) {
        if (write_all(fd, E.rows[i].chars, E.rows[i].size) == -1) {
            ok = false;
            break;
        }
        total += E.rows[i].size;
        if (i + 1 < E.numrows) {
            if (write_all(fd, "\n", 1) == -1) {
                ok = false;
                break;
            }
            total++;
        }
    }
    if (ok && fsync(fd) == -1)
        ok = false;
    if (close(fd) == -1)
        ok = false;
    if (!ok) {
        editor_set_status("Save failed: %s", strerror(errno));
        return false;
    }

    E.dirty = 0;
    editor_set_status("Saved %zu bytes to %s", total, E.filename);
    return true;
}

static void editor_open(const char *filename)
{
    E.filename = strdup(filename);
    if (!E.filename)
        die("strdup");

    FILE *file = fopen(filename, "r");
    if (!file) {
        if (errno == ENOENT) {
            editor_insert_row(0, "", 0);
            return;
        }
        fprintf(stderr, "uno: cannot open %s: %s\n", filename, strerror(errno));
        exit(EXIT_FAILURE);
    }

    char *line = NULL;
    size_t cap = 0;
    ssize_t len;
    bool ended_with_newline = false;
    while ((len = getline(&line, &cap, file)) != -1) {
        ended_with_newline = len > 0 && line[len - 1] == '\n';
        if (ended_with_newline)
            len--;
        editor_insert_row(E.numrows, line, (size_t)len);
    }
    if (ferror(file)) {
        free(line);
        fclose(file);
        die("getline");
    }
    free(line);
    fclose(file);

    if (E.numrows == 0 || ended_with_newline)
        editor_insert_row(E.numrows, "", 0);
    E.dirty = 0;
}

static void editor_move_cursor(int key)
{
    editor_row *row = &E.rows[E.cy];

    switch (key) {
    case KEY_ARROW_LEFT:
        if (E.cx > 0) {
            E.cx--;
        } else if (E.cy > 0) {
            E.cy--;
            E.cx = (int)E.rows[E.cy].size;
        }
        break;
    case KEY_ARROW_RIGHT:
        if ((size_t)E.cx < row->size) {
            E.cx++;
        } else if (E.cy + 1 < E.numrows) {
            E.cy++;
            E.cx = 0;
        }
        break;
    case KEY_ARROW_UP:
        if (E.cy > 0)
            E.cy--;
        break;
    case KEY_ARROW_DOWN:
        if (E.cy + 1 < E.numrows)
            E.cy++;
        break;
    }
    row = &E.rows[E.cy];
    if ((size_t)E.cx > row->size)
        E.cx = (int)row->size;
}

static void editor_confirm_quit(void)
{
    if (!E.dirty) {
        E.quit = true;
        return;
    }

    editor_set_status("Save modified buffer? y=yes n=no c=cancel");
    editor_refresh_screen();
    for (;;) {
        int c = editor_read_key();
        if (c >= 0 && c <= 255)
            c = tolower((unsigned char)c);
        if (c == 'y') {
            if (editor_save())
                E.quit = true;
            return;
        }
        if (c == 'n') {
            E.quit = true;
            return;
        }
        if (c == 'c' || c == '\x1b') {
            editor_set_status("Quit cancelled");
            return;
        }
    }
}

static void editor_process_keypress(void)
{
    int c = editor_read_key();

    switch (c) {
    case CTRL_KEY('x'):
        editor_confirm_quit();
        break;
    case CTRL_KEY('o'):
        (void)editor_save();
        break;
    case CTRL_KEY('w'):
        editor_search(false);
        break;
    case KEY_F3:
        editor_search(true);
        break;
    case CTRL_KEY('\\'):
        editor_replace();
        break;
    case CTRL_KEY('k'):
        editor_cut_line();
        break;
    case CTRL_KEY('u'):
        editor_paste();
        break;
    case '\r':
    case '\n':
        editor_insert_newline();
        break;
    case KEY_BACKSPACE:
    case CTRL_KEY('h'):
        editor_delete_char();
        break;
    case KEY_DELETE:
        editor_delete_forward();
        break;
    case KEY_HOME:
        E.cx = 0;
        break;
    case KEY_END:
        E.cx = (int)E.rows[E.cy].size;
        break;
    case KEY_PAGE_UP:
    case KEY_PAGE_DOWN: {
        E.cy = c == KEY_PAGE_UP ? E.rowoff : E.rowoff + E.screenrows - 1;
        if (E.cy >= E.numrows)
            E.cy = E.numrows - 1;
        int times = E.screenrows;
        while (times--)
            editor_move_cursor(c == KEY_PAGE_UP ? KEY_ARROW_UP : KEY_ARROW_DOWN);
        break;
    }
    case KEY_ARROW_UP:
    case KEY_ARROW_DOWN:
    case KEY_ARROW_LEFT:
    case KEY_ARROW_RIGHT:
        editor_move_cursor(c);
        break;
    case CTRL_KEY('l'):
    case '\x1b':
        break;
    case '\t':
        editor_insert_char('\t');
        break;
    default:
        if ((c >= 32 && c < 127) || (c >= 128 && c <= 255))
            editor_insert_char(c);
        break;
    }
}

static void editor_init(void)
{
    memset(&E, 0, sizeof(E));
    if (get_window_size(&E.screenrows, &E.screencols) == -1) {
        E.screenrows = 24;
        E.screencols = 80;
    }
    E.screenrows = E.screenrows > 2 ? E.screenrows - 2 : 1;
}

static void editor_free(void)
{
    for (int i = 0; i < E.numrows; i++)
        free(E.rows[i].chars);
    free(E.rows);
    free(E.filename);
    free(E.clipboard);
    free(E.last_search);
}

static void print_usage(FILE *out)
{
    fprintf(out, "Usage: uno [FILE]\n"
                 "A small, single-file terminal text editor.\n\n"
                 "  -h, --help       show this help\n"
                 "  -v, --version    show version\n");
}

int main(int argc, char **argv)
{
    if (argc > 2) {
        print_usage(stderr);
        return EXIT_FAILURE;
    }
    if (argc == 2 && (strcmp(argv[1], "-h") == 0 ||
                      strcmp(argv[1], "--help") == 0)) {
        print_usage(stdout);
        return EXIT_SUCCESS;
    }
    if (argc == 2 && (strcmp(argv[1], "-v") == 0 ||
                      strcmp(argv[1], "--version") == 0)) {
        puts("uno " UNO_VERSION);
        return EXIT_SUCCESS;
    }

    editor_init();
    if (argc == 2)
        editor_open(argv[1]);
    else
        editor_insert_row(0, "", 0);
    enable_raw_mode();
    editor_set_status("^O Save | ^X Exit | ^W Find | ^\\ Replace | ^K Cut | ^U Paste");

    while (!E.quit) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    editor_free();
    terminal_write_best_effort("\x1b[2J\x1b[H\x1b[?25h", 13);
    return EXIT_SUCCESS;
}
