/*
 * Minimal ncurses-based screen editor
 * - Launch with `./codein [filename]` (filename optional)
 */

#include <ncurses.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define MAX_LINES 10000
#define MAX_COL 4096
#define MAX_SEARCH 256

#define UNDO_DEPTH 32

static char *lines[MAX_LINES];
static int num_lines = 0;
static char filename[1024] = {0};

static int cur_x = 0; // column index
static int cur_y = 0; // line index
static int top_line = 0; // first visible line

static char search_query[MAX_SEARCH] = {0};

/* forward declarations */
static void newline(void);
static void page_up(void);
static void page_down(void);
static void push_undo(void);
static void do_undo(void);
static void do_redo(void);
static void search_forward(void);
static void prompt_search(void);
static void show_help(void);
static void prompt_save_filename(void);

typedef struct {
    char **lines;
    int num_lines;
    int cur_x, cur_y, top_line;
} UndoSnapshot;

static UndoSnapshot undo_stack[UNDO_DEPTH];
static int undo_count = 0;
static UndoSnapshot redo_stack[UNDO_DEPTH];
static int redo_count = 0;

static void free_snapshot(UndoSnapshot *s)
{
    if (!s->lines) return;
    for (int i = 0; i < s->num_lines; ++i) free(s->lines[i]);
    free(s->lines);
    s->lines = NULL;
    s->num_lines = 0;
}

static void push_undo(void)
{
    if (undo_count == UNDO_DEPTH) {
        // drop oldest
        free_snapshot(&undo_stack[0]);
        memmove(&undo_stack[0], &undo_stack[1], sizeof(UndoSnapshot) * (UNDO_DEPTH - 1));
        undo_count--;
    }
    UndoSnapshot *s = &undo_stack[undo_count];
    s->num_lines = num_lines;
    s->lines = malloc(sizeof(char*) * s->num_lines);
    if (!s->lines) {
        s->num_lines = 0;
        return;
    }
    for (int i = 0; i < s->num_lines; ++i) s->lines[i] = strdup(lines[i]);
    s->cur_x = cur_x; s->cur_y = cur_y; s->top_line = top_line;
    undo_count++;
    // clear redo stack on new action
    for (int i = 0; i < redo_count; ++i) free_snapshot(&redo_stack[i]);
    redo_count = 0;
}

static void do_undo(void)
{
    if (undo_count == 0) {
        beep();
        return;
    }
    // get last snapshot
    UndoSnapshot *s = &undo_stack[undo_count - 1];
    // save current state to redo stack
    if (redo_count < UNDO_DEPTH) {
        UndoSnapshot *r = &redo_stack[redo_count];
        r->num_lines = num_lines;
        r->lines = malloc(sizeof(char*) * r->num_lines);
        if (r->lines) {
            for (int i = 0; i < r->num_lines; ++i) r->lines[i] = strdup(lines[i]);
            r->cur_x = cur_x; r->cur_y = cur_y; r->top_line = top_line;
            redo_count++;
        }
    } else {
        // redo stack full, drop oldest
        free_snapshot(&redo_stack[0]);
        memmove(&redo_stack[0], &redo_stack[1], sizeof(UndoSnapshot) * (UNDO_DEPTH - 1));
        UndoSnapshot *r = &redo_stack[UNDO_DEPTH - 1];
        r->num_lines = num_lines;
        r->lines = malloc(sizeof(char*) * r->num_lines);
        if (r->lines) {
            for (int i = 0; i < r->num_lines; ++i) r->lines[i] = strdup(lines[i]);
            r->cur_x = cur_x; r->cur_y = cur_y; r->top_line = top_line;
        }
    }
    // free current buffer
    for (int i = 0; i < num_lines; ++i) free(lines[i]);
    // copy snapshot into current buffer
    for (int i = 0; i < s->num_lines; ++i) lines[i] = s->lines[i];
    // null out transferred pointers to avoid double-free
    s->lines = NULL;
    num_lines = s->num_lines;
    cur_x = s->cur_x; cur_y = s->cur_y; top_line = s->top_line;
    // remove snapshot from stack
    undo_count--;
}

static void do_redo(void)
{
    if (redo_count == 0) {
        beep();
        return;
    }
    // get last snapshot from redo stack
    UndoSnapshot *r = &redo_stack[redo_count - 1];
    // save current state to undo stack
    if (undo_count < UNDO_DEPTH) {
        UndoSnapshot *s = &undo_stack[undo_count];
        s->num_lines = num_lines;
        s->lines = malloc(sizeof(char*) * s->num_lines);
        if (s->lines) {
            for (int i = 0; i < s->num_lines; ++i) s->lines[i] = strdup(lines[i]);
            s->cur_x = cur_x; s->cur_y = cur_y; s->top_line = top_line;
            undo_count++;
        }
    } else {
        // undo stack full, drop oldest
        free_snapshot(&undo_stack[0]);
        memmove(&undo_stack[0], &undo_stack[1], sizeof(UndoSnapshot) * (UNDO_DEPTH - 1));
        UndoSnapshot *s = &undo_stack[UNDO_DEPTH - 1];
        s->num_lines = num_lines;
        s->lines = malloc(sizeof(char*) * s->num_lines);
        if (s->lines) {
            for (int i = 0; i < s->num_lines; ++i) s->lines[i] = strdup(lines[i]);
            s->cur_x = cur_x; s->cur_y = cur_y; s->top_line = top_line;
        }
    }
    // free current buffer
    for (int i = 0; i < num_lines; ++i) free(lines[i]);
    // copy snapshot into current buffer
    for (int i = 0; i < r->num_lines; ++i) lines[i] = r->lines[i];
    // null out transferred pointers to avoid double-free
    r->lines = NULL;
    num_lines = r->num_lines;
    cur_x = r->cur_x; cur_y = r->cur_y; top_line = r->top_line;
    // remove snapshot from redo stack
    redo_count--;
}

static void search_forward(void)
{
    if (!search_query[0]) {
        beep();
        return;
    }
    int len = strlen(search_query);
    int start_y = cur_y, start_x = cur_x + 1;
    // search from current position forward
    for (int y = start_y; y < num_lines; ++y) {
        char *line = lines[y];
        int search_from = (y == start_y) ? start_x : 0;
        char *p = strstr(line + search_from, search_query);
        if (p) {
            cur_y = y;
            cur_x = p - line;
            return;
        }
    }
    // wrap around: search from beginning
    for (int y = 0; y < start_y; ++y) {
        char *line = lines[y];
        char *p = strstr(line, search_query);
        if (p) {
            cur_y = y;
            cur_x = p - line;
            return;
        }
    }
    beep(); // not found
}

static void prompt_search(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    // draw search input line at bottom
    move(rows - 1, 0);
    clrtoeol();
    attron(A_REVERSE);
    mvprintw(rows - 1, 0, "Search: ");
    attroff(A_REVERSE);
    refresh();
    // collect input until ESC or Enter
    char buf[MAX_SEARCH] = {0};
    int pos = 0;
    int ch;
    while (1) {
        ch = getch();
        if (ch == 27) { // ESC
            break;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            strncpy(search_query, buf, MAX_SEARCH - 1);
            search_forward();
            break;
        } else if (ch == KEY_BACKSPACE || ch == 127) {
            if (pos > 0) buf[--pos] = '\0';
        } else if (ch >= 32 && ch < 127 && pos < MAX_SEARCH - 1) {
            buf[pos++] = (char)ch;
            buf[pos] = '\0';
        }
        // redraw input
        move(rows - 1, 0);
        clrtoeol();
        attron(A_REVERSE);
        mvprintw(rows - 1, 0, "Search: %s", buf);
        attroff(A_REVERSE);
        refresh();
    }
}

static void show_help(void)
{
    erase();
    const char *help_text[] = {
        "=== CODEIN EDITOR HELP ===",
        "",
        "Navigation:",
        "  Arrow Keys      Move cursor",
        "  Page Up/Down    Move by page",
        "  Ctrl+Home       Go to start (not impl)",
        "  Ctrl+End        Go to end (not impl)",
        "",
        "Editing:",
        "  Type            Insert characters",
        "  Backspace       Delete character",
        "  Enter           New line / split line",
        "  Ctrl+U          Undo",
        "  Ctrl+Z          Redo",
        "",
        "Search & File:",
        "  Ctrl+F          Find text",
        "  Ctrl+N          Find next",
        "  Ctrl+S          Save file (prompts for name if none set)",
        "  Ctrl+Q          Quit editor",
        "  Ctrl+H          Show this help",
        "",
        "Press any key to return...",
        NULL
    };

    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int line = 0;
    for (int i = 0; help_text[i] != NULL && line < rows - 1; ++i, ++line) {
        mvprintw(line, 0, "%s", help_text[i]);
    }
    refresh();
    getch(); // wait for any key
}

static void load_file(const char *path)
{
    FILE *f;
    if (!path) {
        lines[0] = strdup("");
        num_lines = 1;
        return;
    }
    strncpy(filename, path, sizeof(filename) - 1);
    f = fopen(path, "r");
    if (!f) {
        lines[0] = strdup("");
        num_lines = 1;
        return;
    }
    char *buf = NULL;
    size_t cap = 0;
    ssize_t len;
    num_lines = 0;
    while ((len = getline(&buf, &cap, f)) != -1 && num_lines < MAX_LINES) {
        // strip newline
        while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) buf[--len] = '\0';
        lines[num_lines++] = strdup(buf);
    }
    free(buf);
    if (num_lines == 0) {
        lines[0] = strdup("");
        num_lines = 1;
    }
    fclose(f);
}

static int save_file(const char *path)
{
    const char *p = path ? path : filename;
    if (!p || p[0] == '\0') return -1;
    FILE *f = fopen(p, "w");
    if (!f) return -1;
    for (int i = 0; i < num_lines; ++i) {
        fprintf(f, "%s\n", lines[i]);
    }
    fclose(f);
    return 0;
}

static void prompt_save_filename(void)
{
    if (filename[0] != '\0') {
        // filename already set, just save
        save_file(filename);
        return;
    }
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    // draw prompt at bottom
    move(rows - 1, 0);
    clrtoeol();
    attron(A_REVERSE);
    mvprintw(rows - 1, 0, "Save as: ");
    attroff(A_REVERSE);
    refresh();
    // collect input until ESC or Enter
    char buf[1024] = {0};
    int pos = 0;
    int ch;
    while (1) {
        ch = getch();
        if (ch == 27) { // ESC - cancel
            break;
        } else if (ch == '\n' || ch == KEY_ENTER) {
            if (pos > 0) {
                strncpy(filename, buf, sizeof(filename) - 1);
                save_file(filename);
            }
            break;
        } else if (ch == KEY_BACKSPACE || ch == 127) {
            if (pos > 0) buf[--pos] = '\0';
        } else if (ch >= 32 && ch < 127 && pos < 1023) {
            buf[pos++] = (char)ch;
            buf[pos] = '\0';
        }
        // redraw input
        move(rows - 1, 0);
        clrtoeol();
        attron(A_REVERSE);
        mvprintw(rows - 1, 0, "Save as: %s", buf);
        attroff(A_REVERSE);
        refresh();
    }
}

static void draw_screen()
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();
    int visible = rows - 1; // reserve last line for status
    for (int i = 0; i < visible; ++i) {
        int idx = top_line + i;
        if (idx >= num_lines) break;
        // highlight current line with underline
        if (idx == cur_y) {
            attron(A_BOLD);
        }
        // only draw up to screen width
        mvaddnstr(i, 0, lines[idx], cols);
        if (idx == cur_y) {
            attroff(A_BOLD);
        }
    }
    // status (truncate if necessary)
    move(rows - 1, 0);
    clrtoeol();
    char status[4096];
    if (filename[0])
        snprintf(status, sizeof(status), "File: %s  Ln %d Col %d  Ctrl-H: help", filename, cur_y+1, cur_x+1);
    else
        snprintf(status, sizeof(status), "[No Name]  Ln %d Col %d  Ctrl-H: help", cur_y+1, cur_x+1);
    attron(A_REVERSE);
    mvaddnstr(rows - 1, 0, status, cols);
    attroff(A_REVERSE);

    // ensure cursor is within visible bounds
    if (cur_x < 0) cur_x = 0;
    if (cur_y < 0) cur_y = 0;
    if (cur_y >= num_lines) cur_y = num_lines - 1;
    int disp_y = cur_y - top_line;
    if (disp_y < 0) {
        top_line = cur_y;
        disp_y = 0;
    } else if (disp_y >= visible) {
        top_line = cur_y - visible + 1;
        disp_y = visible - 1;
    }
    int disp_x = cur_x;
    if (disp_x >= cols) disp_x = cols - 1;
    move(disp_y, disp_x);
    refresh();
}

static void insert_char(int c)
{
    // record undo before mutating
    push_undo();
    // wrap to newline when reaching screen width
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    (void)rows; // rows is unused here; keep to match screen size usage
    if (cur_x >= cols - 1) {
        newline();
        return;
    }
    char *ln = lines[cur_y];
    int len = strlen(ln);
    if (len + 2 >= MAX_COL) return;
    char *newl = malloc(len + 2);
    if (!newl) return;
    memcpy(newl, ln, cur_x);
    newl[cur_x] = (char)c;
    memcpy(newl + cur_x + 1, ln + cur_x, len - cur_x + 1);
    free(lines[cur_y]);
    lines[cur_y] = newl;
    cur_x++;
}

static void backspace()
{
    push_undo();
    if (cur_x > 0) {
        char *ln = lines[cur_y];
        int len = strlen(ln);
        memmove(ln + cur_x - 1, ln + cur_x, len - cur_x + 1);
        cur_x--;
    } else if (cur_y > 0) {
        int prev_len = strlen(lines[cur_y-1]);
        int cur_len = strlen(lines[cur_y]);
        if (prev_len + cur_len + 1 >= MAX_COL) return;
        char *newl = malloc(prev_len + cur_len + 1);
        strcpy(newl, lines[cur_y-1]);
        strcat(newl, lines[cur_y]);
        free(lines[cur_y-1]);
        lines[cur_y-1] = newl;
        free(lines[cur_y]);
        // shift lines up
        for (int i = cur_y; i < num_lines - 1; ++i) lines[i] = lines[i+1];
        num_lines--;
        cur_y--;
        cur_x = prev_len;
    }
}

static void newline()
{
    push_undo();
    if (num_lines + 1 >= MAX_LINES) return;
    char *ln = lines[cur_y];
    // split at cur_x
    char *left = malloc(cur_x + 1);
    char *right = strdup(ln + cur_x);
    if (!left || !right) return;
    memcpy(left, ln, cur_x);
    left[cur_x] = '\0';
    free(lines[cur_y]);
    lines[cur_y] = left;
    // insert right as new line
    for (int i = num_lines; i > cur_y + 1; --i) lines[i] = lines[i-1];
    lines[cur_y+1] = right;
    num_lines++;
    cur_y++;
    cur_x = 0;
}

static void page_up(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int visible = rows - 1;
    if (visible <= 0) visible = 1;
    if (cur_y == 0) {
        top_line = 0;
        return;
    }
    if (cur_y - visible < 0) cur_y = 0;
    else cur_y -= visible;
    if (top_line > cur_y) top_line = cur_y;
    int l = strlen(lines[cur_y]);
    if (cur_x > l) cur_x = l;
}

static void page_down(void)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    int visible = rows - 1;
    if (visible <= 0) visible = 1;
    if (cur_y >= num_lines - 1) {
        top_line = num_lines > visible ? num_lines - visible : 0;
        return;
    }
    if (cur_y + visible >= num_lines - 1) cur_y = num_lines - 1;
    else cur_y += visible;
    int desired_top = cur_y - visible + 1;
    if (desired_top < 0) desired_top = 0;
    top_line = desired_top;
    int l = strlen(lines[cur_y]);
    if (cur_x > l) cur_x = l;
}

int main(int argc, char **argv)
{
    if (argc > 1) load_file(argv[1]);
    else load_file(NULL);

    initscr();
    raw();
    keypad(stdscr, TRUE);
    noecho();
    curs_set(1);

    int ch;
    draw_screen();
    while (1) {
        ch = getch();
        if (ch == 17) { // Ctrl-Q
            break;
        } else if (ch == 19) { // Ctrl-S
            prompt_save_filename();
        } else if (ch == KEY_UP) {
            if (cur_y > 0) {
                cur_y--;
                int l = strlen(lines[cur_y]);
                if (cur_x > l) cur_x = l;
            }
        } else if (ch == KEY_DOWN) {
            if (cur_y < num_lines - 1) {
                cur_y++;
                int l = strlen(lines[cur_y]);
                if (cur_x > l) cur_x = l;
            }
        } else if (ch == KEY_PPAGE) {
            page_up();
        } else if (ch == KEY_NPAGE) {
            page_down();
        } else if (ch == KEY_LEFT) {
            if (cur_x > 0) cur_x--;
            else if (cur_y > 0) {
                cur_y--;
                cur_x = strlen(lines[cur_y]);
            }
        } else if (ch == KEY_RIGHT) {
            int l = strlen(lines[cur_y]);
            if (cur_x < l) cur_x++;
            else if (cur_y < num_lines - 1) {
                cur_y++;
                cur_x = 0;
            }
        } else if (ch == KEY_BACKSPACE || ch == 127) {
            backspace();
        } else if (ch == 21) { // Ctrl-U (undo)
            do_undo();
        } else if (ch == 26) { // Ctrl-Z (redo)
            do_redo();
        } else if (ch == 6) { // Ctrl-F
            prompt_search();
        } else if (ch == 14) { // Ctrl-N (search again)
            search_forward();
        } else if (ch == 8) { // Ctrl-H
            show_help();
        } else if (ch == '\n' || ch == KEY_ENTER) {
            newline();
        } else if (ch >= 32 && ch < 127) {
            insert_char(ch);
        }
        draw_screen();
    }

    endwin();
    return 0;
}