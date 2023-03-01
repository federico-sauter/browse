/*******                                                                ********
********        browse.c                                                ********
********                                                                ********
********    Copyright(c) 2014 by Federico Sauter <fsm@pdp.nl>           ********
********                                                                ********

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <ctype.h>
#include <errno.h>
#include <menu.h>
#include <libgen.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

/*  System utilities */
#define mensure(x) _mensure(x, __LINE__)
static void *_mensure(void *p, int line)
{
	if (p == NULL) {
		endwin();
		fprintf(stderr, "Error: out of memory (line %d)\n", line);
		exit(EXIT_FAILURE);
	}
	return p;
}

static char *editor = "vi";
static void seteditor()
{
	char *ed = getenv("EDITOR");
	if (ed != NULL) {
		editor = ed;
	}
}

#define ensure(x) _ensure(x, __LINE__)
static void _ensure(int rc, int line)
{
	if (rc == -1) {
		fprintf(stderr, "Error: %s (line %d)\n", strerror(errno), line);
		exit(EXIT_FAILURE);
	}
}

static FILE *spawn_child(int argc, char *argv[])
{
	FILE *fp = NULL;
	int fd[2];
	pipe(fd);
	pid_t pid = fork();
	if (pid == -1) {
		perror("fork");
		exit(EXIT_FAILURE);
	} else if (pid == 0) {
		/* child */
		ensure(close(fd[0]));
		ensure(dup2(fd[1], STDOUT_FILENO));
		ensure(close(fd[1]));
		ensure(execvp(argv[1], argv + 1));
	} else {
		/* parent */
		close(fd[1]);
		fp = fdopen(fd[0], "r");
		if (fp == NULL) {
			perror("fdopen");
			exit(EXIT_FAILURE);
		}
	}
	return fp;
}

/* struct match - represents a single grep match line */
#define MATCH_PATH_LEN        256
#define MATCH_DESCRIPTION_LEN 256
struct match {
	char filepath[MATCH_PATH_LEN];
	int  line;
	char *name;
	char description[MATCH_DESCRIPTION_LEN];
};

/* menu contaning the matches found */
static ITEM **items;
static MENU *match_menu;

static size_t matchc, matcha;
static struct match *matchv;

#define MATCH_CHUNK_LEN 32
static void resize_matchv()
{
	if (matchc == matcha) {
		matcha += MATCH_CHUNK_LEN;
		matchv = mensure(realloc(matchv, matcha * sizeof(struct match)));
	}
}

static void init_matchv()
{
	matchc = matcha = 0;
	matchv = NULL;
	resize_matchv();
}

static void open_match(const struct match *m)
{
	char *command;
	asprintf(&command, "%s +%u %s", editor, m->line, m->filepath);
	system(command);
	free(command);
}

/* parse_next_match - reads the next line from the stream and parses it.
 *   This function expects a line as produced by grep -n, i.e.:
 *      <filename>:<linenumber>:<matchtext>
 *   The maximum length for each buffer is observed.
 *   Non-printable characters are replaced.
 *   Returns 0 on success, -1 (or EOF), or 1 if the record could not be parsed.
 */
#define SEPARATOR     ':'
#define TAB_REPL      ' '
#define TAB_STOP      4
#define NONPRINT_REPL '.'
static int parse_next_match(FILE *fp, struct match* m)
{
	char line_num_buf[32];
	char *buffer = m->filepath;
	size_t buffer_avail = sizeof(m->filepath);
	int ch = fgetc(fp);
	int state = 0;
	for ( ; ch != EOF; ch = fgetc(fp)) {
		int appendcount = 0;
		if (ch == SEPARATOR) {
			/* state transition: switch buffers */
			if (state == 0) {
				*buffer = '\0';
				buffer = line_num_buf;
				buffer_avail = sizeof(line_num_buf);
				*buffer = '\0';
				++state;
			} else if (state == 1) {
				*buffer = '\0';
				buffer = m->description;
				buffer_avail = sizeof(m->description);
				*buffer = '\0';
				++state;
			} else {
				appendcount = 1;
			}
		} else if (ch == '\n') {
			*buffer = '\0';
			break;
		} else if (!isprint(ch)) {
			if (ch == '\t') {
				ch = TAB_REPL;
				appendcount = TAB_STOP;
			} else {
				ch = NONPRINT_REPL;
				appendcount = 1;
			}
		} else {
			appendcount = 1;
		}
		for (unsigned i = 0; i < appendcount; ++i) {
			if (!buffer_avail) {
				continue;
			} else if (--buffer_avail == 0) {
				*buffer++ = '\0';
			} else {
				*buffer++ = ch;
			}
		}
	}
	if (state == 2) {
		m->line = atoi(line_num_buf);
		return ch == EOF? EOF : 0;
	}
	return ch == EOF? EOF : 1;
}

/* Curses functions */
#define MATCH_COLOR_FG 1
#define MATCH_COLOR_BG 2
#define FOOTER_COLORS  3
static void init_curses()
{
	initscr();
	keypad(stdscr, TRUE);
	cbreak();
	noecho();
	start_color();
	init_pair(MATCH_COLOR_FG, COLOR_WHITE, COLOR_BLUE);
	init_pair(MATCH_COLOR_BG, COLOR_WHITE, COLOR_BLACK);
	init_pair(FOOTER_COLORS,  COLOR_WHITE, COLOR_GREEN);
}

static void cleanup_curses()
{
	endwin();
	unpost_menu(match_menu);
	for (size_t i = 0; i < matchc; ++i) {
		free(matchv[i].name);
		free_item(items[i]);
	}
	free_menu(match_menu);
	free(items);
	free(matchv);
}

/* View functions */
#define EXIT_HINT     "Hit 'q' to exit  "
#define EXIT_HINT_LEN (sizeof(EXIT_HINT) - 1)
static void display_footer(size_t match_count)
{
	char footer[COLS + 1];
	int len = snprintf(footer, sizeof(footer), "%zu matches", match_count);
	if (len < 1) {
		return;
	}
	int l = COLS - len - EXIT_HINT_LEN;
	if (l < 0) {
		return;
	}
	memset(footer + len, ' ', l);
	memcpy(footer + len + l, EXIT_HINT, EXIT_HINT_LEN);
	footer[COLS] = '\0';
	attron(COLOR_PAIR(FOOTER_COLORS) | A_BOLD);
	mvprintw(LINES - 1, 0, "%s", footer);
	attroff(COLOR_PAIR(FOOTER_COLORS));
}

#define MENU_MARK ">"
static void build_menu(FILE *fp)
{
	/* parse menu contents */
	int rc;
	for (init_matchv(); (rc = parse_next_match(fp, &matchv[matchc])) != EOF; resize_matchv()) {
		matchc += (rc == 0)? 1 : 0;
	}
	if (matchc == 0) {
		free(matchv);
		int status;
		if (wait(&status) == -1) {
			perror("Cannot read child process exit status");
			exit(EXIT_FAILURE);
		}
		if (WEXITSTATUS(status) == 0) {
			fprintf(stderr, "Unable to parse matches. (Did you forget to specify the "
				"'-n' option to grep?)\n");
		} else if (WEXITSTATUS(status) == 1) {
			fprintf(stderr, "No matches.\n");
		}
		exit(WEXITSTATUS(status));
	}
	items = mensure(malloc((matchc + 1) * sizeof(ITEM*)));
	for (size_t i = 0; i < matchc; ++i) {
		asprintf(&matchv[i].name, "%s [%d]", basename(matchv[i].filepath), matchv[i].line);
		items[i] = mensure(new_item(matchv[i].name, matchv[i].description));
		set_item_userptr(items[i], &matchv[i]);
	}
	items[matchc] = NULL;

	/* build curses menu */
	init_curses();
	match_menu = mensure(new_menu(items));
	set_menu_fore(match_menu, COLOR_PAIR(MATCH_COLOR_FG) | A_BOLD);
	set_menu_back(match_menu, COLOR_PAIR(MATCH_COLOR_BG));
	set_menu_mark(match_menu, MENU_MARK);
	set_menu_format(match_menu, LINES - 1, 1);
	display_footer(matchc);
	post_menu(match_menu);
	refresh();
}

#define ENTER  10
#define ESCAPE 27
static void event_loop()
{
	for (int c = 1; c; ) {
		switch(c = getch()) {
		case 'j':
		case KEY_DOWN:
			menu_driver(match_menu, REQ_DOWN_ITEM);
			break;
		case 'k':
		case KEY_UP:
			menu_driver(match_menu, REQ_UP_ITEM);
			break;
		case KEY_NPAGE:
			menu_driver(match_menu, REQ_SCR_DPAGE);
			break;
		case KEY_PPAGE:
			menu_driver(match_menu, REQ_SCR_UPAGE);
			break;
		case ENTER: {
			endwin();
			struct match *entry = (struct match *)item_userptr(current_item(match_menu));
			if (entry == NULL) {
				cleanup_curses();
				fprintf(stderr, "Error: internal error: no entry associated with "
					"match\n");
				exit(EXIT_FAILURE);
			}
			open_match(entry);
			refresh();
			pos_menu_cursor(match_menu);
			break;
		}
		case ESCAPE:
		case 'q':
			c = 0;
			break;
		}
	}
	cleanup_curses();
}

/* Program entry point */
int main(int argc, char *argv[])
{
	if (argc < 2) {
		fprintf(stderr, "Usage: %s <program> [ args ... ]\n", *argv);
		exit(2);
	}
	seteditor();
	build_menu(spawn_child(argc, argv));
	event_loop();
}
