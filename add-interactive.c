#include "add-interactive.h"
#include "cache.h"
#include "commit.h"
#include "color.h"
#include "config.h"
#include "diffcore.h"
#include "refs.h"
#include "revision.h"

#define HEADER_INDENT "      "

#define HEADER_MAXLEN 30

struct adddel { uintmax_t add, del; };

struct file_stat {
	struct hashmap_entry ent;
	struct adddel index, worktree;
	char name[FLEX_ARRAY];
};

struct collection_status {
	int collecting_from_index;

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
	unsigned list_only:1;
	unsigned list_only_file_names:1;
	unsigned immediate:1;
	struct strbuf header;
	const char *prompt;
	const char *header_indent;
	void (*on_eof_fn)(void);
};

struct choice {
	struct hashmap_entry e;
	union {
		void (*command_fn)(void);
		struct {
			struct adddel index, worktree;
		} file;
	} u;
	size_t prefix_length;
	const char *name;
};

enum choice_type {
	FILE_STAT,
	COMMAND
};

struct choices {
	struct choice **choices;
	size_t alloc, nr;
	enum choice_type type;
};
#define CHOICES_INIT { NULL, 0, 0, 0 }

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

static char list_and_choose_colors[][COLOR_MAXLEN] = {
	GIT_COLOR_BOLD_BLUE, /* Prompt */
	GIT_COLOR_BOLD,      /* Header */
	GIT_COLOR_BOLD_RED,  /* Help */
	GIT_COLOR_BOLD_RED,  /* Error */
	GIT_COLOR_RESET      /* Reset */
};

static const char *get_color(enum color_add_i ix)
{
	if (want_color(use_color))
		return list_and_choose_colors[ix];
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
		return color_parse(value, list_and_choose_colors[slot]);
	}

	return git_default_config(var, value, cbdata);
}

static int pathname_equal(const void *unused_cmp_data,
			  const void *entry, const void *entry_or_key,
			  const void *keydata)
{
	const struct file_stat *e1 = entry, *e2 = entry_or_key;
	const char *name = keydata ? keydata : e2->name;

	return strcmp(e1->name, name);
}

static int pathname_cmp(const void *a, const void *b)
{
	struct file_stat *f1 = *((struct file_stat **)a);
	struct file_stat *f2 = *((struct file_stat **)b);

	return strcmp(f1->name, f2->name);
}

static void populate_adddel(struct adddel *ad, uintmax_t add, uintmax_t del)
{
	ad->add = add;
	ad->del = del;
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

	compute_diffstat(options, &stat, q);

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

		if (s->collecting_from_index)
			populate_adddel(&entry->index, stat.files[i]->added,stat.files[i]->deleted);
		else
			populate_adddel(&entry->worktree, stat.files[i]->added, stat.files[i]->deleted);
	}
}

static void collect_changes_worktree(struct collection_status *s)
{
	struct rev_info rev;

	s->collecting_from_index = 0;

	init_revisions(&rev, NULL);
	setup_revisions(0, NULL, &rev, NULL);

	/* Use the max_count field to specify the unmerged stage, against
	 * which the working tree file is compared for an unmerged path.
	 * git diff-files itself when running cmd_diff_files() leaves
	 * rev.max_count untouched to get a normal output (as opposed to
	 * the case when it is told to do --base/--ours/--theirs), so it
	 * ends up passing -1 in this field in such a case.
	 */
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

	s->collecting_from_index = 1;

	init_revisions(&rev, NULL);
	opt.def = s->reference;
	setup_revisions(0, NULL, &rev, &opt);

	rev.diffopt.output_format = DIFF_FORMAT_CALLBACK;
	rev.diffopt.format_callback = collect_changes_cb;
	rev.diffopt.format_callback_data = s;

	run_diff_index(&rev, 1);
}

static int on_unborn_branch(void)
{
	int flags = 0;
	struct object_id oid;
	const char *ref;

	ref = resolve_ref_unsafe("HEAD", RESOLVE_REF_READING, &oid, &flags);
	return !ref && (flags & REF_ISSYMREF);
}

static const char *get_diff_reference(void)
{
	return on_unborn_branch() ? empty_tree_oid_hex() : "HEAD";
}

static struct file_stat **list_modified(struct repository *r,
					const char *filter)
{
	int i = 0;
	int hashmap_size = 0;
	struct collection_status *s = xcalloc(1, sizeof(*s));
	struct hashmap_iter iter;
	struct file_stat **files;
	struct file_stat *entry;

	if (repo_read_index(r) < 0) {
		free(s);
		return NULL;
	}

	s->reference = get_diff_reference();
	hashmap_init(&s->file_map, pathname_equal, NULL, 0);

	if (!filter) {
		collect_changes_index(s);
		collect_changes_worktree(s);
	}
	else if (!strcmp(filter, "index-only"))
		collect_changes_index(s);
	else if (!strcmp(filter, "file-only"))
		collect_changes_worktree(s);
	else
		BUG("unknown filter parameter\n");

	hashmap_size = hashmap_get_size(&s->file_map);

	if (hashmap_size < 1) {
		free(s);
		return NULL;
	}

	hashmap_iter_init(&s->file_map, &iter);

	files = xcalloc(hashmap_size + 1, sizeof(struct file_stat));
	while ((entry = hashmap_iter_next(&iter))) {
		files[i++] = entry;
	}
	QSORT(files, hashmap_size, pathname_cmp);
	files[hashmap_size] = NULL;

	hashmap_free(&s->file_map, 0);
	free(s);
	return files;
}

static void populate_wi_changes(struct strbuf *buf, struct adddel *ad,
					char *no_changes)
{
	if (ad->add || ad->del)
		strbuf_addf(buf, "+%"PRIuMAX"/-%"PRIuMAX,
			    ad->add, ad->del);
	else
		strbuf_addf(buf, "%s", _(no_changes));
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
					     size_t prefix_length,
					     struct choice *item)
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
	int i;
	int j;
	int soft_limit = 0;
	int hard_limit = 4;
	struct hashmap map;

	hashmap_init(&map, map_cmp, NULL, 0);

	for (i = 0; i < data->nr; i++) {
		struct prefix_entry *e = xcalloc(1, sizeof(*e));
		struct prefix_entry *e2;
		e->name = data->choices[i]->name;
		e->item = data->choices[i];

		for (j = soft_limit + 1; j <= hard_limit; j++) {
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
	if (regcomp(regex, pattern, REG_EXTENDED))
		return 0;

	is_valid = prefix &&
		   regexec(regex, prefix, 0, NULL, 0) &&
		   strcmp(prefix, "*") &&
		   strcmp(prefix, "?");
	free(regex);
	return is_valid;
}

/* return a string with the prefix highlighted */
/* for now use square brackets; later might use ANSI colors (underline, bold) */
static void highlight_prefix(struct strbuf *buf, struct choice *item)
{
	if (item->prefix_length <= 0 || !is_valid_prefix(item->name)) {
		strbuf_addstr(buf, item->name);
		return;
	}

	strbuf_addf(buf, "%s%.*s%s%s",
		use_color ? get_color(COLOR_PROMPT) : "[",
		(int)item->prefix_length, item->name,
		use_color ? get_color(COLOR_RESET) : "]",
		item->name + item->prefix_length);
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
	int i;
	struct strbuf print_buf = STRBUF_INIT;
	struct strbuf print = STRBUF_INIT;
	struct strbuf index_changes = STRBUF_INIT;
	struct strbuf worktree_changes = STRBUF_INIT;
	char *chosen_choices = xcalloc(data->nr, sizeof(char *));
	struct choices *results = xcalloc(1, sizeof(*results));
	int chosen_size = 0;
	struct strbuf choice = STRBUF_INIT;
	struct strbuf token = STRBUF_INIT;
	struct strbuf input = STRBUF_INIT;

	if (!data) {
		free(chosen_choices);
		free(results);
		return NULL;
	}

	if (!opts->list_only)
		find_unique_prefixes(data);

top:
	while (1) {
		int j;
		int last_lf = 0;
		const char *prompt_color = get_color(COLOR_PROMPT);
		const char *error_color = get_color(COLOR_ERROR);
		char *token_tmp;
		regex_t *regex_dash_range;
		regex_t *regex_number;
		const char *pattern_dash_range;
		const char *pattern_number;
		const char delim[] = " ,";

		strbuf_reset(&choice);
		strbuf_reset(&token);
		strbuf_reset(&input);

		if (opts->header.len) {
			const char *header_color = get_color(COLOR_HEADER);
			if (opts->header_indent)
				fputs(opts->header_indent, stdout);
			color_fprintf_ln(stdout, header_color, "%s", opts->header.buf);
		}

		for (i = 0; i < data->nr; i++) {
			struct choice *c = data->choices[i];
			char chosen = chosen_choices[i]? '*' : ' ';
			const char *modified_fmt = _("%12s %12s %s");
			
			strbuf_reset(&print_buf);

			if (!opts->list_only)
				highlight_prefix(&print_buf, data->choices[i]);
			else
				strbuf_add(&print_buf, c->name, strlen(c->name));
			
			if ((data->type == FILE_STAT) && (!opts->list_only_file_names)) {
				strbuf_reset(&print);
				strbuf_reset(&index_changes);
				strbuf_reset(&worktree_changes);

				populate_wi_changes(&worktree_changes, &c->u.file.worktree,
						    "nothing");
				populate_wi_changes(&index_changes, &c->u.file.index,
						    "unchanged");

				strbuf_addbuf(&print, &print_buf);
				strbuf_reset(&print_buf);
				strbuf_addf(&print_buf, modified_fmt, index_changes.buf,
					       worktree_changes.buf, print.buf);
			}

			printf("%c%2d: %s", chosen, i + 1, print_buf.buf);

			if ((opts->column_n) && ((i + 1) % (opts->column_n))) {
				putchar('\t');
				last_lf = 0;
			}
			else {
				putchar('\n');
				last_lf = 1;
			}
		}

		if (!last_lf)
			putchar('\n');

		if (opts->list_only)
			return NULL;

		color_fprintf(stdout, prompt_color, "%s", opts->prompt);
		if(opts->singleton)
			fputs("> ", stdout);
		else
			fputs(">> ", stdout);

		fflush(stdout);
		strbuf_getline(&input, stdin);
		strbuf_trim(&input);

		if (!input.buf)
			break;
		
		if (!input.buf[0]) {
			putchar('\n');
			if (opts->on_eof_fn)
				opts->on_eof_fn();
			break;
		}

		if (!strcmp(input.buf, "?")) {
			opts->singleton? singleton_prompt_help_cmd() : prompt_help_cmd();
			goto top;
		}

		token_tmp = strtok(input.buf, delim);
		strbuf_add(&token, token_tmp, strlen(token_tmp));

		while (1) {
			int choose = 1;
			int bottom = 0, top = 0;
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

			for (j = bottom - 1; j <= top - 1; j++) {
				if (data->nr <= j || j < 0)
					continue;
				chosen_choices[j] = choose;
				if (choose == 1)
					chosen_size++;
			}

			strbuf_reset(&token);
			strbuf_reset(&choice);

			token_tmp = strtok(NULL, delim);
			if (!token_tmp)
				break;
			strbuf_add(&token, token_tmp, strlen(token_tmp));
		}

		if ((opts->immediate) || !(strcmp(choice.buf, "*")))
			break;
	}

	strbuf_release(&print_buf);
	strbuf_release(&print);
	strbuf_release(&index_changes);
	strbuf_release(&worktree_changes);

	strbuf_release(&choice);
	strbuf_release(&token);
	strbuf_release(&input);

	for (i = 0; i < data->nr; i++) {
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

static struct choice *add_choice(struct choices *choices,
				 struct file_stat *file, struct command *command)
{
	struct choice *choice;

	if (file && command)
		BUG("either file_stat or command should be NULL\n");

	switch (choices->type) {
	case FILE_STAT:
		choice = make_choice(file->name);
		choice->u.file.index = file->index;
		choice->u.file.worktree = file->worktree;
		break;
	case COMMAND:
		choice = make_choice(command->name);
		choice->u.command_fn = command->command_fn;
		break;
	}

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
	int i;
	struct file_stat **files;
	struct list_and_choose_options opts = { 0 };
	struct choices choices = CHOICES_INIT;
	const char *modified_fmt = _("%12s %12s %s");

	choices.type = FILE_STAT;

	opts.list_only = 1;
	opts.header_indent = HEADER_INDENT;
	strbuf_init(&opts.header, 0);
	strbuf_addf(&opts.header, modified_fmt,  _("staged"),
		    _("unstaged"), _("path"));

	files = list_modified(the_repository, NULL);
	if (files == NULL) {
		strbuf_release(&opts.header);
		putchar('\n');
		return;
	}

	for (i = 0; files[i]; i++)
		add_choice(&choices, files[i], NULL);

	list_and_choose(&choices, &opts);
	putchar('\n');

	strbuf_release(&opts.header);
	free(files);
	free_choices(&choices);
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
}
