#include "add-interactive.h"
#include "cache.h"
#include "commit.h"
#include "color.h"
#include "config.h"
#include "diffcore.h"
#include "prompt.h"
#include "revision.h"

#define HEADER_INDENT "      "

#define HEADER_MAXLEN 30

#define COMMAND_NUM 2

enum collection_phase {
	WORKTREE,
	INDEX
};

struct file_stat {
	struct hashmap_entry ent;
	struct {
		uintmax_t added, deleted;
	} index, worktree;
	char name[FLEX_ARRAY];
};

struct collection_status {
	enum collection_phase phase;

	const char *reference;
	struct pathspec pathspec;

	struct hashmap file_map;
};

struct command {
	char *name;
	void (*command_fn)(void);
};

// struct command command_list[] =
// {
// 	{ "status" , &add_i_status },
// 	{ "help" , &add_i_show_help },
// 	{ NULL, NULL }
// };

struct list_and_choose_options {
	int column_n;
	unsigned singleton:1;
	unsigned list_flat:1;
	unsigned list_only:1;
	unsigned immediate:1;
	char *header;
	const char *prompt;
	void (*on_eof_fn)(void);
};

struct prefix_entry {
	struct hashmap_entry e;
	const char *label;
	size_t prefix_length;
	struct stuff_item *item;
};

struct stuff_item {
	const char *label;
	size_t prefix_length;
};

static int use_color = -1;
enum color_add_i {
	COLOR_PROMPT,
	COLOR_HEADER,
	COLOR_HELP,
	COLOR_ERROR,
	COLOR_RESET
};

static char colors[][COLOR_MAXLEN] = {
	GIT_COLOR_BOLD_BLUE, /* Prompt */
	GIT_COLOR_BOLD,      /* Header */
	GIT_COLOR_BOLD_RED,  /* Help */
	GIT_COLOR_BOLD_RED,  /* Error */
	GIT_COLOR_RESET      /* Reset */
};

static const char *get_color(enum color_add_i ix)
{
	if (want_color(use_color))
		return colors[ix];
	return "";
}

static int parse_color_slot(const char *slot)
{
	if (!strcasecmp(slot, "prompt"))
		return COLOR_PROMPT;
	if (!strcasecmp(slot, "header"))
		return COLOR_HEADER;
	if (!strcasecmp(slot, "help"))
		return COLOR_HELP;
	if (!strcasecmp(slot, "error"))
		return COLOR_ERROR;
	if (!strcasecmp(slot, "reset"))
		return COLOR_RESET;

	return -1;
}

int add_i_config(const char *var,
		const char *value, void *cbdata)
{
	const char *name;

	if (!strcmp(var, "color.interactive")) {
		use_color = git_config_colorbool(var, value);
		return 0;
	}

	if (skip_prefix(var, "color.interactive.", &name)) {
		int slot = parse_color_slot(name);
		if (slot < 0)
			return 0;
		if (!value)
			return config_error_nonbool(var);
		return color_parse(value, colors[slot]);
	}

	return git_default_config(var, value, cbdata);
}

static int hash_cmp(const void *unused_cmp_data, const void *entry,
			const void *entry_or_key, const void *keydata)
{
	const struct file_stat *e1 = entry, *e2 = entry_or_key;
	const char *name = keydata ? keydata : e2->name;

	return strcmp(e1->name, name);
}

static int alphabetical_cmp(const void *a, const void *b)
{
	struct file_stat *f1 = *((struct file_stat **)a);
	struct file_stat *f2 = *((struct file_stat **)b);

	return strcmp(f1->name, f2->name);
}

static void collect_changes_cb(struct diff_queue_struct *q,
					 struct diff_options *options,
					 void *data)
{
	struct collection_status *s = data;
	struct diffstat_t stat = { 0 };
	int i;

	if (!q->nr)
		return;

	compute_diffstat(options, &stat);

	for (i = 0; i < stat.nr; i++) {
		struct file_stat *entry;
		const char *name = stat.files[i]->name;
		unsigned int hash = strhash(name);

		entry = hashmap_get_from_hash(&s->file_map, hash, name);
		if (!entry) {
			FLEX_ALLOC_STR(entry, name, name);
			hashmap_entry_init(entry, hash);
			hashmap_add(&s->file_map, entry);
		}

		if (s->phase == WORKTREE) {
			entry->worktree.added = stat.files[i]->added;
			entry->worktree.deleted = stat.files[i]->deleted;
		} else if (s->phase == INDEX) {
			entry->index.added = stat.files[i]->added;
			entry->index.deleted = stat.files[i]->deleted;
		}
	}
}

static void collect_changes_worktree(struct collection_status *s)
{
	struct rev_info rev;

	s->phase = WORKTREE;

	init_revisions(&rev, NULL);
	setup_revisions(0, NULL, &rev, NULL);

	rev.max_count = 0;

	rev.diffopt.flags.ignore_dirty_submodules = 1;
	rev.diffopt.output_format = DIFF_FORMAT_CALLBACK;
	rev.diffopt.format_callback = collect_changes_cb;
	rev.diffopt.format_callback_data = s;

	run_diff_files(&rev, 0);
}

static void collect_changes_index(struct collection_status *s)
{
	struct rev_info rev;
	struct setup_revision_opt opt = { 0 };

	s->phase = INDEX;

	init_revisions(&rev, NULL);
	opt.def = s->reference;
	setup_revisions(0, NULL, &rev, &opt);

	rev.diffopt.output_format = DIFF_FORMAT_CALLBACK;
	rev.diffopt.format_callback = collect_changes_cb;
	rev.diffopt.format_callback_data = s;

	run_diff_index(&rev, 1);
}

static int is_inital_commit(void)
{
	struct object_id sha1;
	if (get_oid("HEAD", &sha1))
		return 1;
	return 0;
}

static const char *get_diff_reference(void)
{
	if(is_inital_commit())
		return empty_tree_oid_hex();
	return "HEAD";
}

static void filter_files(const char *filter, struct hashmap *file_map,
						struct file_stat **files)
{

	for (int i = 0; i < hashmap_get_size(file_map); i++) {
		struct file_stat *f = files[i];

		if ((!(f->worktree.added || f->worktree.deleted)) &&
		   (!strcmp(filter, "file-only")))
				hashmap_remove(file_map, f, NULL);

		if ((!(f->index.added || f->index.deleted)) &&
		   (!strcmp(filter, "index-only")))
				hashmap_remove(file_map, f, NULL);
	}
}

static struct collection_status *print_modified(const char *filter)
{
	int i = 0;
	struct collection_status *s = xcalloc(1, sizeof(struct collection_status *));
	struct hashmap_iter iter;
	struct file_stat **files;
	struct file_stat *entry;

	if (read_cache() < 0) {
		printf("\n");
		return NULL;
	}

	s->reference = get_diff_reference();
	hashmap_init(&s->file_map, hash_cmp, NULL, 0);

	collect_changes_worktree(s);
	collect_changes_index(s);

	if (hashmap_get_size(&s->file_map) < 1) {
		printf("\n");
		return NULL;
	}

	hashmap_iter_init(&s->file_map, &iter);

	files = xcalloc(hashmap_get_size(&s->file_map), sizeof(struct file_stat *));
	while ((entry = hashmap_iter_next(&iter))) {
		files[i++] = entry;
	}
	QSORT(files, hashmap_get_size(&s->file_map), alphabetical_cmp);

	if (filter)
		filter_files(filter, &s->file_map, files);

	free(files);
	return s;
}

static int find_unique(char *string, struct stuff_item **stuff,
						int size)
{
	int found = 0;
	int i = 0;
	int hit = 0;

	for (i = 0; i < size; i++) {
		struct stuff_item *item = stuff[i];
		hit = 0;
		if (!strcmp(item->label, string))
			hit = 1;
		if (hit && found)
			return 0;
		if (hit)
			found = i + 1;
	}

	return found;
}

/* filters out prefixes which have special meaning to list_and_choose() */
static int is_valid_prefix(const char *prefix)
{
	regex_t *regex;
	const char *pattern = "(\\s,)|(^-)|(^[0-9]+)";
	int is_valid = 0;

	regex = xmalloc(sizeof(*regex));
	if (regcomp(regex, pattern, REG_EXTENDED)) {
		return 0;
	}

	is_valid = prefix &&
			   regexec(regex, prefix, 0, NULL, 0) &&
			   strcmp(prefix, "*") &&
			   strcmp(prefix, "?");
	free(regex);
	return is_valid;
}

/* return a string with the prefix highlighted */
/* for now use square brackets; later might use ANSI colors (underline, bold) */
static char *highlight_prefix(struct stuff_item *item)
{
	struct strbuf buf;
	struct strbuf prefix;
	struct strbuf remainder;
	const char *prompt_color = get_color(COLOR_PROMPT);
	const char *reset_color = get_color(COLOR_RESET);
	int remainder_size = strlen(item->label) - item->prefix_length;

	strbuf_init(&buf, 0);

	strbuf_init(&prefix, 0);
	strbuf_add(&prefix, item->label, item->prefix_length);

	strbuf_init(&remainder, 0);
	strbuf_add(&remainder, item->label + item->prefix_length,
			  remainder_size + 1);

	if(!prefix.buf) {
		strbuf_release(&buf);
		strbuf_release(&prefix);
		return remainder.buf;
	}
	
	if (!is_valid_prefix(prefix.buf)) {
		strbuf_addstr(&buf, prefix.buf);
		strbuf_addstr(&buf, remainder.buf);
	}
	else if (!use_color) {
		strbuf_addstr(&buf, "[");
		strbuf_addstr(&buf, prefix.buf);
		strbuf_addstr(&buf, "]");
		strbuf_addstr(&buf, remainder.buf);
	}
	else {
		strbuf_addstr(&buf, prompt_color);
		strbuf_addstr(&buf, prefix.buf);
		strbuf_addstr(&buf, reset_color);
		strbuf_addstr(&buf, remainder.buf);
	}

	strbuf_release(&prefix);
	strbuf_release(&remainder);

	return buf.buf;
}

static struct prefix_entry *new_prefix_entry(const char *label,
				 size_t prefix_length, struct stuff_item *item)
{
	struct prefix_entry *result = xcalloc(1, sizeof(*result));
	result->label = label;
	result->prefix_length = prefix_length;
	result->item = item;
	hashmap_entry_init(result, memhash(label, prefix_length));
	return result;
}

static int map_cmp(const void *unused_cmp_data,
				  const void *entry,
				  const void *entry_or_key,
				  const void *unused_keydata)
{
	const struct prefix_entry *a = entry;
	const struct prefix_entry *b = entry_or_key;
	if((a->prefix_length == b->prefix_length) &&
	  (strncmp(a->label, b->label, a->prefix_length) == 0))
		return 0;
	return 1;
}

static void find_unique_prefixes(struct stuff_item **stuff, int size)
{
	int soft_limit = 0;
	int hard_limit = 4;
	struct hashmap map;

	hashmap_init(&map, map_cmp, NULL, 0);

	for (int i = 0; i < size; i++) {
		struct prefix_entry *e = xcalloc(1, sizeof(*e));
		struct prefix_entry *e2;
		e->label = stuff[i]->label;
		e->item = stuff[i];

		for (int j = soft_limit + 1; j <= hard_limit; j++) {
			/* We only allow alphanumerical prefixes */
			// if (!isalnum(e->label[j]))
			// 	break;
			if (!isascii(e->label[j]))
				break;

			e->prefix_length = j;
			hashmap_entry_init(e, memhash(e->label, j));
			e2 = hashmap_get(&map, e, NULL);
			if (!e2) {
				e->item->prefix_length = j;
				hashmap_add(&map, e);
				e = NULL;
				break;
			}

			if (!e2->item) {
				continue; /* non-unique prefix */
			}

			if (j != e2->item->prefix_length)
				BUG("Hashmap entry has unexpected prefix length (%"PRIuMAX"/ != %"PRIuMAX"/)",
				   (uintmax_t)j, (uintmax_t)e2->item->prefix_length);

			/* skip common prefix */
			for (j++; j <= hard_limit && e->label[j - 1]; j++) {
				if (e->item->label[j - 1] != e2->item->label[j - 1])
					break;
				hashmap_add(&map, new_prefix_entry(e->label, j, NULL));
			}
			if (j <= hard_limit && e2->label[j - 1]) {
				e2->item->prefix_length = j;
				hashmap_add(&map, new_prefix_entry(e2->label, j, e2->item));
			}
			else {
				e2->item->prefix_length = 0;
			}
			e2->item = NULL;

			if (j <= hard_limit && e->label[j - 1]) {
				e->item->prefix_length = j;
				hashmap_add(&map, new_prefix_entry(e->label,
						   e->item->prefix_length, e->item));
				e = NULL;
			}
			else
				e->item->prefix_length = 0;
			break;
		}

		free(e);
	}
}

static void singleton_prompt_help_cmd(void)
{
	const char *help_color = get_color(COLOR_HELP);
	const char help_singleton_prompt[] =
		N_("Prompt help:\n"
		"1          - select a numbered item\n"
		"foo        - select item based on unique prefix\n"
		"           - (empty) select nothing");
	color_fprintf_ln(stdout, help_color, "%s", _(help_singleton_prompt));
}

static void prompt_help_cmd(void)
{
	const char *help_color = get_color(COLOR_HELP);
	const char help_prompt[] =
		N_("Prompt help:\n"
		"1          - select a single item\n"
		"3-5        - select a range of items\n"
		"2-3,6-9    - select multiple ranges\n"
		"foo        - select item based on unique prefix\n"
		"-...       - unselect specified items\n"
		"*          - choose all items\n"
		"           - (empty) finish selecting");
	color_fprintf(stdout, help_color, "%s", _(help_prompt));
}

static struct stuff_item **get_stuff_from_commands(struct stuff_item **items,
												  struct command **commands)
{
	for(int i = 0; commands[i]->name; i++) {
		struct stuff_item *item = xcalloc(1, sizeof(struct stuff_item *));
		item->label = commands[i]->name;
		item->prefix_length = 0;
		items[i] = item;
	}

	return items;
}

static struct stuff_item **get_stuff_from_collection(struct stuff_item **items,
									 struct collection_status *collection,
									 int size)
{
	int i = 0;
	struct hashmap_iter iter;
	struct file_stat *entry;
	hashmap_iter_init(&collection->file_map, &iter);

	while ((entry = hashmap_iter_next(&iter))) {
		struct stuff_item *item = xcalloc(1, sizeof(struct stuff_item *));
		item->label = entry->name;
		item->prefix_length = 0;
		items[i] = item;
		i++;
	}

	return items;
}

static void *list_and_choose(const char type, void *data, int size,
					   struct list_and_choose_options *opts)
{
	struct stuff_item **prefixes = NULL;
	struct file_stat **files = NULL;
	struct file_stat **chosen_files = NULL;
	struct collection_status *collection = NULL;
	struct command **commands = NULL;
	char *chosen_list = xcalloc(size, sizeof(char *));
	int i = 0;
	int chosen_size = 0;
	struct hashmap_iter iter;
	struct file_stat *entry;

	if (!data) {
		free(chosen_list);
		return NULL;
	}

	switch(type) {
		case 'c':
			commands = xcalloc(size, sizeof(struct command *));
			commands[0]->name = "status";
			commands[0]->command_fn = &add_i_status;
			commands[1]->name = "help";
			commands[1]->command_fn = &add_i_show_help;
			break;
		case 'f':
			collection = data;
			hashmap_iter_init(&collection->file_map, &iter);
			files = xcalloc(hashmap_get_size(&collection->file_map),
						   sizeof(struct file_stat *));
			while ((entry = hashmap_iter_next(&iter))) {
				files[i++] = entry;
			}
			QSORT(files, hashmap_get_size(&collection->file_map), alphabetical_cmp);
			break;
	}

	if (!opts->list_only) {
		switch(type) {
		case 'c':
			prefixes = xcalloc(COMMAND_NUM, sizeof(struct stuff_item *));
			get_stuff_from_commands(prefixes, commands);
			break;
		case 'f':
			prefixes = xcalloc(size, sizeof(struct stuff_item *));
			get_stuff_from_collection(prefixes, collection, size);
			break;
		}
		find_unique_prefixes(prefixes, size);
	}

top:
	while (1) {
		int last_lf = 0;
		const char *prompt_color = get_color(COLOR_PROMPT);
		struct strbuf input = STRBUF_INIT;
		char *choice = NULL;
		char *token;
		regex_t *regex_dash_range;
		regex_t *regex_number;
		const char *pattern_dash_range;
		const char *pattern_number;
		const char delim[] = " ,";

		if (opts->header) {
			const char *header_color = get_color(COLOR_HEADER);
			if (!opts->list_flat)
				printf(HEADER_INDENT);
			/* check: is header translatable? in .perl script it is not */
			color_fprintf_ln(stdout, header_color, "%s", opts->header);
		}

		for (int i = 0; i < size; i++) {
			char chosen = chosen_list[i]? '*' : ' ';
			char *print;
			char *highlighted;

			if (prefixes)
				highlighted = highlight_prefix(prefixes[i]);

			if (commands)
				print = highlighted? highlighted : commands[i]->name;

			if (files != NULL) {
				char worktree_changes[50];
				char index_changes[50];
				char print_buf[100];
				struct file_stat *f = files[i];
				const char *modified_fmt = _("%12s %12s %s");

				if (f->worktree.added || f->worktree.deleted)
					snprintf(worktree_changes, 50, "+%"PRIuMAX"/-%"PRIuMAX,
							f->worktree.added, f->worktree.deleted);
				else
					snprintf(worktree_changes, 50, "%s", _("nothing"));

				if (f->index.added || f->index.deleted)
					snprintf(index_changes, 50, "+%"PRIuMAX"/-%"PRIuMAX,
							f->index.added, f->index.deleted);
				else
					snprintf(index_changes, 50, "%s", _("unchanged"));

				if (!prefixes)
					highlighted = f->name;
				
				snprintf(print_buf, 100, modified_fmt, index_changes,
						worktree_changes, highlighted);
				print = xmalloc(strlen(print_buf) + 1);
				snprintf(print, 100, "%s", print_buf);
			}

			printf("%c%2d: %s", chosen, i + 1, print);

			if ((opts->list_flat) && ((i+1) % (opts->list_flat))) {
				printf("\t");
				last_lf = 0;
			}
			else {
				printf("\n");
				last_lf = 1;
			}

		}/* end for loop */

		if (!last_lf)
			printf("\n");

		if (opts->list_only)
			return NULL;

		color_fprintf(stdout, prompt_color, "%s", opts->prompt);
		if(opts->singleton)
			printf("> ");
		else
			printf(">> ");

		// if (!(strbuf_read(&input, 0, 0) < 0)) {
		// 	printf("\n");
		// 	if (opts->on_eof_fn)
		// 		opts->on_eof_fn();
		// 	break;
		// }

		if (strbuf_getline_lf(&input, stdin) != EOF)
			strbuf_trim(&input);

		if (!input.buf) {
			printf("buffer is empty\n");
			break;
		}
		
		if (!strcmp(input.buf, "\n")) {
			printf("\n");
			printf("buffer is newline\n");
			strbuf_trim(&input);
			if (opts->on_eof_fn)
				opts->on_eof_fn();
			break;
		}

		printf("buffer (%s) is == ? : %d\n", input.buf, !strcmp(input.buf, "?"));

		if (!strcmp(input.buf, "?")) {
			opts->singleton? singleton_prompt_help_cmd() : prompt_help_cmd();
			goto top;
		}

		token = strtok(input.buf, delim);
		for (int j = 0; token != NULL; j++) {
			int choose = 1;
			int bottom = 0, top = 0;
			choice[0] = token[0];
			choice[1] = '\0';

			/* Input that begins with '-'; unchoose */
			pattern_dash_range = "^-";
			regex_dash_range = xmalloc(sizeof(*regex_dash_range));

			if (regcomp(regex_dash_range, pattern_dash_range, REG_EXTENDED))
				BUG("regex compilation for pattern %s failed",
				   pattern_dash_range);
			if (!regexec(regex_dash_range, choice, 0, NULL, 0))
				choose = 0;


			/* A range can be specified like 5-7 or 5-. */
			pattern_dash_range = "^(\\d+)-(\\d*)";
			pattern_number = "^\\d+$";
			regex_number = xmalloc(sizeof(*regex_number));

			if (regcomp(regex_dash_range, pattern_dash_range, REG_EXTENDED))
				BUG("regex compilation for pattern %s failed",
				   pattern_dash_range);
			if (regcomp(regex_number, pattern_number, REG_EXTENDED))
				BUG("regex compilation for pattern %s failed", pattern_number);

			if (!regexec(regex_dash_range, choice, 0, NULL, 0)) { 
				const char delim_dash[] = "-";
				char *num = NULL;
				num = strtok(choice, delim_dash);
				bottom = atoi(num);
				num = strtok(NULL, delim_dash);
				top = num? atoi(num) : (1 + size);
			}
			else if (!regexec(regex_number, choice, 0, NULL, 0)) {
				bottom = top = atoi(choice);
			}
			else if (!strcmp(choice, "*")) {
				bottom = 1;
				top = 1 + size;
			}
			else {
				bottom = top = find_unique(choice, prefixes, size);
				if (!bottom) {
					error(_("Huh (%s)?\n"), choice);
					goto top;
				}
			}

			if (opts->singleton && bottom != top) {
				error(_("Huh (%s)?\n"), choice);
				goto top;
			}

			for (int i = bottom - 1; i < top - 1; i++) {
				if (size <= i || i < 0)
					continue;
				chosen_list[i] = choose;
				if (choose == 1)
					chosen_size++;
			}

			token = strtok(NULL, delim);
		}

		if ((opts->immediate) || !(strcmp(input.buf, "*")))
			break; //last
	}

	if (type == 'f')
		chosen_files = xcalloc(chosen_size, sizeof(struct file_stat *));

	for (int i = 0, j = 0; i < size; i++) {
		if (chosen_list[i]) {
			if (type == 'f') {
				chosen_files[j] = files[i];
				j++;
			}
			else if (type == 'c')
				return commands[i];
		}
	}

	if (type == 'f')
		return chosen_files[0];

	return NULL;
}

void add_i_status(void)
{
	struct collection_status *s;
	struct list_and_choose_options opts;
	const char *modified_fmt = _("%12s %12s %s");
	const char type = 'f';

	opts.list_only = 1;
	opts.header = xmalloc(sizeof(char) * (HEADER_MAXLEN + 1));

	snprintf(opts.header, HEADER_MAXLEN + 1, modified_fmt,
			_("staged"), _("unstaged"), _("path"));

	s = print_modified(NULL);
	if (s == NULL)
		return;

	list_and_choose(type, s, hashmap_get_size(&s->file_map), &opts);

	free(&s->file_map);
	free(s);

}

static char *check_input(void)
{
	// struct strbuf input = STRBUF_INIT;
	// strbuf_getline_lf(&input, stdin);
	// return input.buf;
	char *answer = git_prompt(_("My question? "), PROMPT_ECHO);
	return answer;
}

void add_i_show_help(void)
{
	const char *help_color = get_color(COLOR_HELP);
	color_fprintf_ln(stdout, help_color, "status        - %s",
			_("show paths with changes"));
	color_fprintf_ln(stdout, help_color, "update        - %s",
			_("add working tree state to the staged set of changes"));
	color_fprintf_ln(stdout, help_color, "revert        - %s",
			_("revert staged set of changes back to the HEAD version"));
	color_fprintf_ln(stdout, help_color, "patch         - %s",
			_("pick hunks and update selectively"));
	color_fprintf_ln(stdout, help_color, "diff          - %s",
			_("view diff between HEAD and index"));
	color_fprintf_ln(stdout, help_color, "add untracked - %s",
			_("add contents of untracked files to the staged set of changes"));

	printf("%s\n", check_input());

}
