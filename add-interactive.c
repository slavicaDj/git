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

static int use_color = -1;
enum color_add_i {
	COLOR_PROMPT,
	COLOR_HEADER,
	COLOR_HELP,
	COLOR_ERROR
};

static char colors[][COLOR_MAXLEN] = {
	GIT_COLOR_BOLD_BLUE, /* Prompt */
	GIT_COLOR_BOLD,      /* Header */
	GIT_COLOR_BOLD_RED,  /* Help */
	GIT_COLOR_BOLD_RED   /* Error */
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

static struct collection_status *print_modified(const char *filter,
				 struct list_and_choose_options *opts)
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

void add_i_status(void)
{
	int i = 0;
	struct collection_status *s;
	struct hashmap *map;
	struct hashmap_iter iter;
	struct file_stat *f;
	struct list_and_choose_options opts;
	const char *modified_fmt = _("%12s %12s %s");
	const char *header_color = get_color(COLOR_HEADER);

	opts.list_only = 1;
	opts.header = xmalloc(sizeof(char) * (HEADER_MAXLEN + 1));

	snprintf(opts.header, HEADER_MAXLEN + 1, modified_fmt,
			_("staged"), _("unstaged"), _("path"));

	s = print_modified(NULL, &opts);
	if (s == NULL)
		return;
	
	map = &s->file_map;

	printf(HEADER_INDENT);
	color_fprintf_ln(stdout, header_color, "%s", opts.header);

	hashmap_iter_init(map, &iter);
	while ((f = hashmap_iter_next(&iter))) {

		char worktree_changes[50];
		char index_changes[50];

		if (f->worktree.added || f->worktree.deleted)
			snprintf(worktree_changes, 50, "+%"PRIuMAX"/-%"PRIuMAX, f->worktree.added,
					f->worktree.deleted);
		else
			snprintf(worktree_changes, 50, "%s", _("nothing"));

		if (f->index.added || f->index.deleted)
			snprintf(index_changes, 50, "+%"PRIuMAX"/-%"PRIuMAX, f->index.added,
					f->index.deleted);
		else
			snprintf(index_changes, 50, "%s", _("unchanged"));

		printf(" %2d: ", i + 1);
		printf(modified_fmt, index_changes, worktree_changes, f->name);
		printf("\n");
		i++;
	}
	printf("\n");
	free(map);
	free(s);
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
