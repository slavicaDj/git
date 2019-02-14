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
