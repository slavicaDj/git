#include "add-interactive.h"
#include "cache.h"
#include "commit.h"
#include "color.h"
#include "config.h"
#include "diffcore.h"
#include "regex.h"
#include "revision.h"

#define HEADER_INDENT "      "

/* TRANSLATORS: please do not translate the command names
   'status', 'update', 'revert', etc. */
static const char help_info[] = 
		N_("status        - show paths with changes\n"
		"update        - add working tree state to the staged set of changes\n"
		"revert        - revert staged set of changes back to the HEAD version\n"
		"patch         - pick hunks and update selectively\n"
		"diff          - view diff between HEAD and index\n"
		"add untracked - add contents of untracked files to the staged set of changes");

// static const char help_singleton_prompt[] =
// 		N_("Prompt help:\n"
// 		"1          - select a numbered item\n"
// 		"foo        - select item based on unique prefix\n"
// 		"           - (empty) select nothing");

// static const char help_prompt[] =
// 		N_("Prompt help:\n"
// 		"1          - select a single item\n"
// 		"3-5        - select a range of items\n"
// 		"2-3,6-9    - select multiple ranges\n"
// 		"foo        - select item based on unique prefix\n"
// 		"-...       - unselect specified items\n"
// 		"*          - choose all items\n"
// 		"           - (empty) finish selecting");

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

// struct command {
// 	const char *name;
// 	void (*command_fn)(void);
// };

// static struct command command_list[] = {
// 	{ "status", &add_i_print_modified },
// 	{ "help", &show_help },
// 	{ NULL, NULL }
// };

// struct list_and_choose_options {
// 	int column_n;
// 	unsigned singleton:1;
// 	unsigned list_flat:1;
// 	unsigned list_only:1;
// 	unsigned immediate:1;
// 	const char *header;
// 	const char *prompt;
// 	void (*on_eof_fn)(void);
// };

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

void add_i_print_modified(void)
{
	int i = 0;
	struct collection_status s;
	/* TRANSLATORS: you can adjust this to align "git add -i" status menu */
	const char *modified_fmt = _("%12s %12s %s");
	const char *header_color = get_color(COLOR_HEADER);

	struct hashmap_iter iter;
	struct file_stat **files;
	struct file_stat *entry;

	if (read_cache() < 0)
		return;

	s.reference = get_diff_reference();
	hashmap_init(&s.file_map, hash_cmp, NULL, 0);

	collect_changes_worktree(&s);
	collect_changes_index(&s);

	if (hashmap_get_size(&s.file_map) < 1) {
		printf("\n");
		return;
	}

	printf(HEADER_INDENT);
	color_fprintf(stdout, header_color, modified_fmt, _("staged"),
			_("unstaged"), _("path"));
	printf("\n");

	hashmap_iter_init(&s.file_map, &iter);

	files = xcalloc(hashmap_get_size(&s.file_map), sizeof(struct file_stat *));
	while ((entry = hashmap_iter_next(&iter))) {
		files[i++] = entry;
	}
	QSORT(files, hashmap_get_size(&s.file_map), alphabetical_cmp);

	for (i = 0; i < hashmap_get_size(&s.file_map); i++) {
		struct file_stat *f = files[i];

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
	}
	printf("\n");

	free(files);
	hashmap_free(&s.file_map, 1);
}

// static int find_unique_file_stat(char *string, struct file_stat **files, 
// 						int size)
// {
// 	int found = 0;
// 	int i = 0;
// 	int hit = 0;

// 	for (i = 0; i < size; i++) {
// 		struct file_stat *f = files[i];
// 		hit = 0;
// 		if (!strcmp(f->name, string))
// 			hit = 1;
// 		if (hit && found)
// 			return 0;
// 		if (hit)
// 			found = i + 1;
// 	}

// 	return found;
// }

// static int find_unique_command(char *string)
// {
// 	struct command *c;
// 	int found = 0;
// 	int i = 0;
// 	int hit = 0;

// 	for (c = command_list; c->name; c++, i++) {
// 		hit = 0;
// 		if (starts_with(c->name, string))
// 			hit = 1;
// 		if (hit && found)
// 			return 0;
// 		if (hit)
// 			found = i + 1;
// 	}

// 	return found;
// }

/* Filters out prefixes which have special meaning to list_and_choose() */
// static int is_valid_prefix(const char *prefix)
// {
// 	regex_t *regex;
// 	const char *pattern = "(\\s,)|(^-)|(^[0-9]+)";
// 	int is_valid = 0;

// 	regex = xmalloc(sizeof(*regex));
// 	if (regcomp(regex, pattern, REG_EXTENDED)) {
// 		return 0;
// 	}

// 	is_valid = prefix &&
// 			   regexec(regex, prefix, 0, NULL, 0) &&
// 			   strcmp(prefix, "*") &&
// 			   strcmp(prefix, "?");

// 	return is_valid;
// }

// static char *highlight_prefix(char *prefix, char *remainder)
// {
// 	struct strbuf highlight;
// 	const char *prompt_color = get_color(COLOR_PROMPT);
// 	const char *reset_color = get_color(COLOR_RESET);

// 	if(!prefix)
// 		return remainder;
	
// 	if (!is_valid_prefix(prefix)) {
// 		strbuf_init(&highlight, 0);
// 		strbuf_addstr(&highlight, prefix);
// 		strbuf_addstr(&highlight, remainder);
// 		return highlight.buf;
// 	}
	
// 	if (!use_color) {
// 		strbuf_init(&highlight, 0);
// 		strbuf_addstr(&highlight, "[");
// 		strbuf_addstr(&highlight, prefix);
// 		strbuf_addstr(&highlight, "]");
// 		strbuf_addstr(&highlight, remainder);
// 		return highlight.buf;
// 	}

// 	strbuf_init(&highlight, 0);
// 	strbuf_addstr(&highlight, prompt_color);
// 	strbuf_addstr(&highlight, prefix);
// 	strbuf_addstr(&highlight, reset_color);
// 	strbuf_addstr(&highlight, remainder);
// 	return highlight.buf;
// }

static int map_cmp(const void *unused_cmp_data,
				  const void *entry,
				  const void *entry_or_key,
				  const void *unused_keydata)
{
	const struct prefix_entry *a = entry;
	const struct prefix_entry *b = entry_or_key;
	if((a->prefix_length == b->prefix_length) &&
	  (strncmp(a->label, b->label, a->prefix_length) == 0)) {
		return 0;
	}
	return 1;
}

static void print_hashmap(struct hashmap *map)
{
	struct hashmap_iter iter;
	struct prefix_entry *entry;

	hashmap_iter_init(map, &iter);
	while ((entry = hashmap_iter_next(&iter))) {
		printf("--------------------\n");
		printf("hash: %d\n", entry->e.hash);
		printf("%s\n", entry->label);
		printf("%d\n", entry->prefix_length);
		if (entry->item) {
			printf("item is not null:\n");
			printf("%s\n", entry->item->label);
			printf("%d\n", entry->item->prefix_length);
		}
		else {
			printf("item is null\n");
		}
		printf("--------------------\n");
	}
}

static void find_unique_prefixes(struct stuff_item *stuff, int size)
{
	int soft_limit = 0;
	int hard_limit = 4;
	struct hashmap map;

	hashmap_init(&map, map_cmp, NULL, 0);

	for (int i = 0; i < size; i++) {
		struct prefix_entry *e = xcalloc(1, sizeof(*e));
		struct prefix_entry *e2 = NULL;
		e->label = stuff[i].label;
		e->item = &stuff[i];
		for (int j = soft_limit + 1; j <= hard_limit; j++) {
			/* this doesn't work if negation is not used
			i.e. if (isalnum(e->label[j])) returns false */
			if (!isalnum(e->label[j])) {
				break;
			}
			e->prefix_length = j;
			hashmap_entry_init(e, memhash(e->label, j));
			e2 = hashmap_get(&map, e, NULL);
			if (!e2) {
				/* simple case, works good */
				e->item->prefix_length = j;
				hashmap_add(&map, e);
				e = NULL;
				break;
			}
			else {
				/* if the entry has item != NULL, compare with that one, 
				find the shortest prefix length for which they are different 
				(or drop out if the maximum length was reached, in which case 
				we need to invalidate the item's prefix_length e.g. by 
				setting it to 0 and then 
				setting the existing prefix_entry's item to NULL) */

				int index = 0; /* represents index in label and later on
								new_prefix_length = index + 1 */
				struct prefix_entry *new_e2 = xcalloc(1, sizeof(*new_e2));

				/* Johannes suggested skipping items with prefix length != j; 
				I fail to understand when that might happen,
				since we either set entry's item to NULL or set it's prefix_length
				to be the same as item's prefix_length. 
				Although, since segfault happens in the line below when 
				I try to set entry's item to NULL, described scenario (when entry
				and its item have different prefix_length) actually happens,
				although that wasn't my intention (I followed algorithm Johannes
				described) */

				// if (j != e2->item->prefix_length) {
				// 	free(new_e2);
				// 	continue;
				// }

				if (e2->item != NULL) {
					struct prefix_entry *entry = xcalloc(1, sizeof(*entry));
					/* while loop terminated in 3 cases:
					1 - we come to the end of e->item->label
					2 - hard limit is exceeded
					3 - we find character at which e->item->label and
					e2->item->label starting to be different */
					while (e->item->label[index] != 0) {
						struct prefix_entry *found_entry = NULL;
						if (e->item->label[index] != 
						   e2->item->label[index]) {
							if (index == hard_limit - 1) {
								e2->item->prefix_length = 0;
								e2->item = NULL;
								free(e);
								free(new_e2);
								free(entry);
								BUG("hard limit exceeded for %s's prefix.", e->label);
							}
							break;
						}
						/* for all prefixes of the two strings e->item->label
						and e2->item->label that are the same we either:
						1 - manage to find such entry and set its item to NULL
						2 - fail to find such entry, then add it to hashmap
						with item set to NULL */
						entry->label = e2->item->label;
						entry->prefix_length = index + 1;
						hashmap_entry_init(entry, memhash(entry->label, 
														  entry->prefix_length));
						found_entry = hashmap_get(&map, entry, NULL);
						if (found_entry) {
							printf("entry found\n");
							printf("%s %d %s %d\n", found_entry->label, found_entry->prefix_length,
							found_entry->item->label, found_entry->item->prefix_length);
							// found_entry->item = NULL; /* causes segfault when uncommented */
						}
						else {
							/* entry is NULL */
							hashmap_add(&map, entry);
							entry = NULL;
						}
						index++;
					}
					free(entry);

					/* make new entries with prefix length determined with index,
					i.e. index + 1, because that is where e->item->label 
					and e2->item->label start to be different; 
					for example: snow & stars -> add (snow, 2) and (stars, 2)
								 (snow, 1) has been previously invalidated by
								 setting its item to NULL 
								 (if segfault weren't to happen) */
					e->item->prefix_length = index + 1;
					e->prefix_length = index + 1;
					new_e2->item = e2->item;
					new_e2->item->prefix_length = index + 1;
					new_e2->label = new_e2->item->label;
					new_e2->prefix_length = index + 1;
					hashmap_entry_init(e, memhash(e->label, e->prefix_length));
					hashmap_add(&map, e);
					e = NULL;
					hashmap_entry_init(new_e2, memhash(new_e2->label, new_e2->prefix_length));
					hashmap_add(&map, new_e2);
					new_e2 = NULL;

					/*
					- status -> s,1 already in hashmap
					storm
					- j = 1 -> we find a match with item != NULL
					- we compare status and storm and conclude there
					are starting to be different from third character
					- this means we need to invalidate s,1 by setting its item
					to NULL
					- we also need to add three more entries in hashmap:
						-st, 2 pointing to NULL
						-sta,3 pointing to status
						-sto,3 pointing to storm
					*/
				break;
				}
				free(new_e2);
			}
		}
		free(e);
	}
	print_hashmap(&map);
}

void show_help(void)
{
	const char *help_color = get_color(COLOR_HELP);
	const char *modified_fmt = _("%s");
	struct stuff_item stuff[] = 
	{
		/* for these 5 items, prefixes
		are determined correctly */
		{ "status", 0 },
		{ "help", 0 },
		{ "revert", 0 },
		{ "steady", 0 },
		{ "add untracked", 0 },
		{ NULL, 0 }
		/* when including "still",
		segfault happens */
		// { "still", 0 }
	};
	printf("\n");
	color_fprintf(stdout, help_color, modified_fmt, _(help_info));
	printf("\n");

	find_unique_prefixes(stuff, 5);

	for (int i = 0; stuff[i].label != NULL; i++) {
		printf("%d\n", stuff[i].prefix_length);
	}
}

// static void singleton_prompt_help_cmd(void)
// {
// 	const char *help_color = get_color(COLOR_HELP);
// 	const char *modified_fmt = _("%s");
// 	printf("\n");
// 	color_fprintf(stdout, help_color, modified_fmt, _(help_singleton_prompt));
// 	printf("\n");
// }

// static void prompt_help_cmd(void)
// {
// 	const char *help_color = get_color(COLOR_HELP);
// 	const char *modified_fmt = _("%s");
// 	printf("\n");
// 	color_fprintf(stdout, help_color, modified_fmt, _(help_prompt));
// 	printf("\n");
// }

