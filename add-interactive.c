#include "add-interactive.h"
#include "cache.h"
#include "commit.h"
#include "color.h"
#include "config.h"
#include "diffcore.h"
#include "revision.h"

#define HEADER_INDENT "      "

#define HEADER_MAXLEN 30

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

struct list_and_choose_options {
	int column_n;
	unsigned singleton:1;
	unsigned list_flat:1;
	unsigned list_only:1;
	unsigned list_only_file_names:1;
	unsigned immediate:1;
	char *header;
	const char *prompt;
	void (*on_eof_fn)(void);
};

struct choice {
	struct hashmap_entry e;
	char type;
	union {
		void (*command_fn)(void);
		struct {
			struct {
				uintmax_t added, deleted;
			} index, worktree;
		} file;
	} u;
	size_t prefix_length;
	const char *name;
};

struct choices {
	struct choice **choices;
	size_t alloc, nr;
};
#define CHOICES_INIT { NULL, 0, 0 }

struct prefix_entry {
	struct hashmap_entry e;
	const char *name;
	size_t prefix_length;
	struct choice *item;
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

static struct collection_status *list_modified(const char *filter)
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

static int map_cmp(const void *unused_cmp_data,
				  const void *entry,
				  const void *entry_or_key,
				  const void *unused_keydata)
{
	const struct choice *a = entry;
	const struct choice *b = entry_or_key;
	if((a->prefix_length == b->prefix_length) &&
	  (strncmp(a->name, b->name, a->prefix_length) == 0))
		return 0;
	return 1;
}

static struct prefix_entry *new_prefix_entry(const char *name,
				 size_t prefix_length, struct choice *item)
{
	struct prefix_entry *result = xcalloc(1, sizeof(*result));
	result->name = name;
	result->prefix_length = prefix_length;
	result->item = item;
	hashmap_entry_init(result, memhash(name, prefix_length));
	return result;
}

static void find_unique_prefixes(struct choices *data)
{
	int soft_limit = 0;
	int hard_limit = 4;
	struct hashmap map;

	hashmap_init(&map, map_cmp, NULL, 0);

	for (int i = 0; i < data->nr; i++) {
		struct prefix_entry *e = xcalloc(1, sizeof(*e));
		struct prefix_entry *e2;
		e->name = data->choices[i]->name;
		e->item = data->choices[i];

		for (int j = soft_limit + 1; j <= hard_limit; j++) {
			if (!isascii(e->name[j]))
				break;

			e->prefix_length = j;
			hashmap_entry_init(e, memhash(e->name, j));
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
			for (j++; j <= hard_limit && e->name[j - 1]; j++) {
				if (e->item->name[j - 1] != e2->item->name[j - 1])
					break;
				hashmap_add(&map, new_prefix_entry(e->name, j, NULL));
			}
			if (j <= hard_limit && e2->name[j - 1]) {
				e2->item->prefix_length = j;
				hashmap_add(&map, new_prefix_entry(e2->name, j, e2->item));
			}
			else {
				e2->item->prefix_length = 0;
			}
			e2->item = NULL;

			if (j <= hard_limit && e->name[j - 1]) {
				e->item->prefix_length = j;
				hashmap_add(&map, new_prefix_entry(e->name,
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

static int find_unique(char *string, struct choices *data)
{
	int found = 0;
	int i = 0;
	int hit = 0;

	for (i = 0; i < data->nr; i++) {
		struct choice *item = data->choices[i];
		hit = 0;
		if (!strcmp(item->name, string))
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
static char *highlight_prefix(struct choice *item)
{
	struct strbuf buf;
	struct strbuf prefix;
	struct strbuf remainder;
	const char *prompt_color = get_color(COLOR_PROMPT);
	const char *reset_color = get_color(COLOR_RESET);
	int remainder_size = strlen(item->name) - item->prefix_length;

	strbuf_init(&buf, 0);

	strbuf_init(&prefix, 0);
	strbuf_add(&prefix, item->name, item->prefix_length);

	strbuf_init(&remainder, 0);
	strbuf_add(&remainder, item->name + item->prefix_length,
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

static void singleton_prompt_help_cmd(void)
{
	const char *help_color = get_color(COLOR_HELP);
	color_fprintf_ln(stdout, help_color, "%s", _("Prompt help:"));
	color_fprintf_ln(stdout, help_color, "1          - %s",
					_("select a numbered item"));
	color_fprintf_ln(stdout, help_color, "foo        - %s",
					_("select item based on unique prefix"));
	color_fprintf_ln(stdout, help_color, "           - %s",
					_("(empty) select nothing"));
}

static void prompt_help_cmd(void)
{
	const char *help_color = get_color(COLOR_HELP);
	color_fprintf_ln(stdout, help_color, "%s",
					_("Prompt help:"));
	color_fprintf_ln(stdout, help_color, "1          - %s",
					_("select a single item"));
	color_fprintf_ln(stdout, help_color, "3-5        - %s",
					_("select a range of items"));
	color_fprintf_ln(stdout, help_color, "2-3,6-9    - %s",
					_("select multiple ranges"));
	color_fprintf_ln(stdout, help_color, "foo        - %s",
					_("select item based on unique prefix"));
	color_fprintf_ln(stdout, help_color, "-...       - %s",
					_("unselect specified items"));
	color_fprintf_ln(stdout, help_color, "*          - %s",
					_("choose all items"));
	color_fprintf_ln(stdout, help_color, "           - %s",
					_("(empty) finish selecting"));
}

static struct choices *list_and_choose(struct choices *data,
							struct list_and_choose_options *opts)
{
	char *chosen_choices = xcalloc(data->nr, sizeof(char *));
	struct choices *results = xcalloc(1, sizeof(*results));
	int chosen_size = 0;

	if (!data) {
		free(chosen_choices);
		free(results);
		return NULL;
	}
	
	if (!opts->list_only)
		find_unique_prefixes(data);

top:
	while (1) {
		int last_lf = 0;
		const char *prompt_color = get_color(COLOR_PROMPT);
		const char *error_color = get_color(COLOR_ERROR);
		struct strbuf input = STRBUF_INIT;
		struct strbuf choice;
		struct strbuf token;
		char *token_tmp;
		regex_t *regex_dash_range;
		regex_t *regex_number;
		const char *pattern_dash_range;
		const char *pattern_number;
		const char delim[] = " ,";

		if (opts->header) {
			const char *header_color = get_color(COLOR_HEADER);
			if (!opts->list_flat)
				printf(HEADER_INDENT);
			color_fprintf_ln(stdout, header_color, "%s", opts->header);
		}

		for (int i = 0; i < data->nr; i++) {
			struct choice *c = data->choices[i];
			char chosen = chosen_choices[i]? '*' : ' ';
			char *print;
			const char *modified_fmt = _("%12s %12s %s");
			char worktree_changes[50];
			char index_changes[50];
			char print_buf[100];

			if (!opts->list_only)
				print = highlight_prefix(data->choices[i]);
			else
				print = (char *)c->name;
			
			if ((data->choices[i]->type == 'f') && (!opts->list_only_file_names)) {
				uintmax_t worktree_added = c->u.file.worktree.added;
				uintmax_t worktree_deleted = c->u.file.worktree.deleted;
				uintmax_t index_added = c->u.file.index.added;
				uintmax_t index_deleted = c->u.file.index.deleted;

				if (worktree_added || worktree_deleted)
					snprintf(worktree_changes, 50, "+%"PRIuMAX"/-%"PRIuMAX,
							worktree_added, worktree_deleted);
				else
					snprintf(worktree_changes, 50, "%s", _("nothing"));

				if (index_added || index_deleted)
					snprintf(index_changes, 50, "+%"PRIuMAX"/-%"PRIuMAX,
							index_added, index_deleted);
				else
					snprintf(index_changes, 50, "%s", _("unchanged"));

				snprintf(print_buf, 100, modified_fmt, index_changes,
						worktree_changes, print);
				print = xmalloc(strlen(print_buf) + 1);
				snprintf(print, 100, "%s", print_buf);
			}

			printf("%c%2d: %s", chosen, i + 1, print);

			if ((opts->list_flat) && ((i + 1) % (opts->column_n))) {
				printf("\t");
				last_lf = 0;
			}
			else {
				printf("\n");
				last_lf = 1;
			}

		}

		if (!last_lf)
			printf("\n");

		if (opts->list_only)
			return NULL;

		color_fprintf(stdout, prompt_color, "%s", opts->prompt);
		if(opts->singleton)
			printf("> ");
		else
			printf(">> ");

		fflush(stdout);
		strbuf_getline(&input, stdin);
		strbuf_trim(&input);

		if (!input.buf)
			break;
		
		if (!input.buf[0]) {
			printf("\n");
			if (opts->on_eof_fn)
				opts->on_eof_fn();
			break;
		}

		if (!strcmp(input.buf, "?")) {
			opts->singleton? singleton_prompt_help_cmd() : prompt_help_cmd();
			goto top;
		}

		token_tmp = strtok(input.buf, delim);
		strbuf_init(&token, 0);
		strbuf_add(&token, token_tmp, strlen(token_tmp));

		while (1) {
			int choose = 1;
			int bottom = 0, top = 0;
			strbuf_init(&choice, 0);
			strbuf_addbuf(&choice, &token);

			/* Input that begins with '-'; unchoose */
			pattern_dash_range = "^-";
			regex_dash_range = xmalloc(sizeof(*regex_dash_range));

			if (regcomp(regex_dash_range, pattern_dash_range, REG_EXTENDED))
				BUG("regex compilation for pattern %s failed",
				   pattern_dash_range);
			if (!regexec(regex_dash_range, choice.buf, 0, NULL, 0)) {
				choose = 0;
				/* remove dash from input */
				strbuf_remove(&choice, 0, 1);
			}

			/* A range can be specified like 5-7 or 5-. */
			pattern_dash_range = "^([0-9]+)-([0-9]*)$";
			pattern_number = "^[0-9]+$";
			regex_number = xmalloc(sizeof(*regex_number));

			if (regcomp(regex_dash_range, pattern_dash_range, REG_EXTENDED))
				BUG("regex compilation for pattern %s failed",
				   pattern_dash_range);
			if (regcomp(regex_number, pattern_number, REG_EXTENDED))
				BUG("regex compilation for pattern %s failed", pattern_number);

			if (!regexec(regex_dash_range, choice.buf, 0, NULL, 0)) {
				const char delim_dash[] = "-";
				char *num = NULL;
				num = strtok(choice.buf, delim_dash);
				bottom = atoi(num);
				num = strtok(NULL, delim_dash);
				top = num? atoi(num) : (1 + data->nr);
			}
			else if (!regexec(regex_number, choice.buf, 0, NULL, 0))
				bottom = top = atoi(choice.buf);
			else if (!strcmp(choice.buf, "*")) {
				bottom = 1;
				top = 1 + data->nr;
			}
			else {
				bottom = top = find_unique(choice.buf, data);
				if (!bottom) {
					color_fprintf_ln(stdout, error_color, _("Huh (%s)?"), choice.buf);
					goto top;
				}
			}

			if (opts->singleton && bottom != top) {
				color_fprintf_ln(stdout, error_color, _("Huh (%s)?"), choice.buf);
				goto top;
			}

			for (int i = bottom - 1; i <= top - 1; i++) {
				if (data->nr <= i || i < 0)
					continue;
				chosen_choices[i] = choose;
				if (choose == 1)
					chosen_size++;
			}

			strbuf_release(&token);
			strbuf_release(&choice);

			token_tmp = strtok(NULL, delim);
			if (!token_tmp)
				break;
			strbuf_init(&token, 0);
			strbuf_add(&token, token_tmp, strlen(token_tmp));
		}

		if ((opts->immediate) || !(strcmp(choice.buf, "*")))
			break;
	}

	for (int i = 0; i < data->nr; i++) {
		if (chosen_choices[i]) {
			ALLOC_GROW(results->choices, results->nr + 1, results->alloc);
			results->choices[results->nr++] = data->choices[i];
		}
	}

	free(chosen_choices);
	return results;
}

static struct choice *make_choice(const char *name )
{
	struct choice *choice;
	FLEXPTR_ALLOC_STR(choice, name, name);
	return choice;
}

static struct choice *add_choice(struct choices *choices, const char type,
								struct file_stat *file, struct command *command)
{
	struct choice *choice;
	switch (type) {
		case 'f':
			choice = make_choice(file->name);
			choice->u.file.index.added = file->index.added;
			choice->u.file.index.deleted = file->index.deleted;
			choice->u.file.worktree.added = file->worktree.added;
			choice->u.file.worktree.deleted = file->worktree.deleted;
			break;
		case 'c':
			choice = make_choice(command->name);
			choice->u.command_fn = command->command_fn;
			break;
	}
	choice->type = type;

	ALLOC_GROW(choices->choices, choices->nr + 1, choices->alloc);
	choices->choices[choices->nr++] = choice;

	return choice;
}

static void free_choices(struct choices *choices)
{
	int i;

	for (i = 0; i < choices->nr; i++)
		free(choices->choices[i]);
	free(choices->choices);
	choices->choices = NULL;
	choices->nr = choices->alloc = 0;
}

void add_i_status(void)
{
	struct collection_status *s;
	struct list_and_choose_options opts;
	struct hashmap *map;
	struct hashmap_iter iter;
	struct choices choices = CHOICES_INIT;
	struct file_stat *entry;
	const char *modified_fmt = _("%12s %12s %s");
	const char type = 'f';

	opts.list_only = 1;
	opts.header = xmalloc(sizeof(char) * (HEADER_MAXLEN + 1));
	snprintf(opts.header, HEADER_MAXLEN + 1, modified_fmt,
			_("staged"), _("unstaged"), _("path"));

	s = list_modified(NULL);
	if (s == NULL)
		return;

	map = &s->file_map;
	hashmap_iter_init(map, &iter);
	while ((entry = hashmap_iter_next(&iter))) {
		add_choice(&choices, type, entry, NULL);
	}

	list_and_choose(&choices, &opts);

	free(&s->file_map);
	free(s);
	free_choices(&choices);
}
