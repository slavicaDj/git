#include "add-interactive.h"
#include "builtin.h"
#include "config.h"
#include "revision.h"

static const char * const builtin_add_helper_usage[] = {
	N_("git add-interactive--helper <command>"),
	NULL
};

enum cmd_mode {
	DEFAULT = 0,
	STATUS,
	HELP
};

int cmd_add__helper(int argc, const char **argv, const char *prefix)
{
	enum cmd_mode mode = DEFAULT;

	struct option options[] = {
		OPT_CMDMODE(0, "status", &mode,
			 N_("print status information with diffstat"), STATUS),
		OPT_CMDMODE(0, "show-help", &mode,
			 N_("show help"), HELP),
		OPT_END()
	};

	git_config(add_i_config, NULL);
	argc = parse_options(argc, argv, NULL, options,
			     builtin_add_helper_usage,
			     PARSE_OPT_KEEP_ARGV0);

	if (mode == STATUS)
		add_i_status();
	else if (mode == HELP)
		add_i_show_help();
	else
		usage_with_options(builtin_add_helper_usage,
				   options);

	return 0;
}
