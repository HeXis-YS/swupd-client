/*
 *   Software Updater - client side
 *
 *      Copyright © 2019-2025 Intel Corporation.
 *
 *   This program is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, version 2 or later of the License.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "3rd_party_repos.h"
#include "swupd.h"

#include <errno.h>
#include <sys/stat.h>

#ifdef THIRDPARTY

#define FLAG_DOWNLOAD_ONLY 2000

static int cmdline_option_version = -1;
static bool cmdline_option_download_only = false;
static bool cmdline_option_keepcache = false;
static bool cmdline_option_status = false;
static char *cmdline_option_repo = NULL;

static void print_help(void)
{
	print("Performs a system software update for content installed from 3rd-party repositories\n\n");
	print("Usage:\n");
	print("   swupd 3rd-party update [OPTION...]\n\n");

	global_print_help();

	print("Options:\n");
	print("   -R, --repo              Specify the 3rd-party repository to use\n");
	print("   -V, --version=V         Update to version V, also accepts 'latest' (default)\n");
	print("   -s, --status            Show current OS version and latest version available on server. Equivalent to \"swupd check-update\"\n");
	print("   -k, --keepcache         Do not delete the swupd state directory content after updating the system\n");
	print("   --download              Download all content, but do not actually install the update\n");
	print("\n");
}

static const struct option prog_opts[] = {
	{ "download", no_argument, 0, FLAG_DOWNLOAD_ONLY },
	{ "version", required_argument, 0, 'V' },
	{ "status", no_argument, 0, 's' },
	{ "keepcache", no_argument, 0, 'k' },
	{ "repo", required_argument, 0, 'R' },
};

static bool parse_opt(int opt, char *optarg)
{
	int err;

	switch (opt) {
	case 'V':
		if (str_cmp("latest", optarg) == 0) {
			cmdline_option_version = -1;
			return true;
		}

		err = str_to_int(optarg, &cmdline_option_version);
		if (err < 0 || cmdline_option_version < 0) {
			error("Invalid --version argument: %s\n\n", optarg);
			return false;
		}
		return true;
	case 's':
		cmdline_option_status = optarg_to_bool(optarg);
		return true;
	case 'k':
		cmdline_option_keepcache = optarg_to_bool(optarg);
		return true;
	case FLAG_DOWNLOAD_ONLY:
		cmdline_option_download_only = optarg_to_bool(optarg);
		return true;
	case 'R':
		cmdline_option_repo = strdup_or_die(optarg);
		return true;
	default:
		return false;
	}

	return false;
}

static const struct global_options opts = {
	prog_opts,
	sizeof(prog_opts) / sizeof(struct option),
	parse_opt,
	print_help,
};

static bool parse_options(int argc, char **argv)
{
	int ind = global_parse_options(argc, argv, &opts);

	if (ind < 0) {
		return false;
	}

	if (argc > ind) {
		error("unexpected arguments\n\n");
		return false;
	}

	/* flag restrictions */
	if (cmdline_option_version > 0) {
		if (!cmdline_option_repo) {
			error("a repository needs to be specified to use the --version flag\n\n");
			return false;
		}
	}

	return true;
}

static enum swupd_code validate_permissions(struct file *file)
{
	enum swupd_code ret_code = SWUPD_OK;
	struct stat file_stats;
	struct stat original_file_stats;
	char *staged_file = NULL;
	char *original_file = NULL;

	if (!file || file->is_deleted) {
		return ret_code;
	}

	staged_file = statedir_get_staged_file(file->hash);
	if (lstat(staged_file, &file_stats) == 0) {
		/* see if the file being updated has dangerous flags */
		if ((file_stats.st_mode & S_ISUID) || (file_stats.st_mode & S_ISGID) || (file_stats.st_mode & S_ISVTX)) {
			if (!file->peer) {
				/* a new file included in the update has dangerous flags */
				warn("The update has a new file %s with dangerous permissions\n", file->filename);
				ret_code = SWUPD_NO;
			} else {
				/* an existing file has dangerous flags, do not warn unless
				 * the flags changed from non-dangerous to dangerous in the update */
				original_file = sys_path_join("%s/%s", globals.path_prefix, file->filename);
				if (lstat(original_file, &original_file_stats) == 0) {
					if (
					    ((file_stats.st_mode & S_ISUID) && !(original_file_stats.st_mode & S_ISUID)) ||
					    ((file_stats.st_mode & S_ISGID) && !(original_file_stats.st_mode & S_ISGID)) ||
					    ((file_stats.st_mode & S_ISVTX) && !(original_file_stats.st_mode & S_ISVTX))) {
						warn("The update sets dangerous permissions to file %s\n", file->filename);
						ret_code = SWUPD_NO;
					}
				} else {
					ret_code = SWUPD_INVALID_FILE;
				}
			}
		}
	} else {
		ret_code = SWUPD_INVALID_FILE;
	}

	FREE(staged_file);
	FREE(original_file);

	return ret_code;
}

static enum swupd_code validate_file_permissions(struct list *files_to_be_updated)
{
	static enum swupd_code ret_code = SWUPD_OK;

	ret_code = third_party_process_files(files_to_be_updated, "\nValidating 3rd-party bundle file permissions...\n", "validate_file_permissions", validate_permissions);
	if (ret_code) {
		if (ret_code == SWUPD_NO) {
			/* the bundle has files with dangerous permissions,
			 * ask the user wether to continue or not */
			warn("\nThe 3rd-party update you are about to install contains files with dangerous permission\n");
			if (confirm_action()) {
				ret_code = SWUPD_OK;
			} else {
				ret_code = SWUPD_INVALID_FILE;
			}
		}
	}

	return ret_code;
}

static enum swupd_code regenerate_all_wrapper_scripts(UNUSED_PARAM char *unused)
{
	enum swupd_code ret_code;
	struct list *current_subs = NULL;
	struct manifest *current_mom = NULL;
	int version;

	/* get currently installed 3rd-party bundles */
	read_subscriptions(&current_subs);

	/* load the MoM */
	version = get_current_version(globals.path_prefix);
	current_mom = load_mom(version, NULL);
	if (!current_mom) {
		ret_code = SWUPD_COULDNT_LOAD_MOM;
		goto exit;
	}

	/* get a list of all 3rd-party files installed */
	current_mom->submanifests = recurse_manifest(current_mom, current_subs, NULL, false, NULL);
	if (!current_mom->submanifests) {
		ret_code = SWUPD_RECURSE_MANIFEST;
		goto exit;
	}
	current_mom->files = consolidate_files_from_bundles(current_mom->submanifests);

	ret_code = third_party_process_files(current_mom->files, "Regenerating scripts...\n", "update_binaries", third_party_update_wrapper_script);
	if (ret_code == SWUPD_OK) {
		info("Scripts regenerated successfully\n");
	}

exit:
	manifest_free(current_mom);
	free_subscriptions(&current_subs);
	return ret_code;
}

static enum swupd_code update_exported_binaries(struct list *updated_files)
{
	return third_party_process_files(updated_files, "\nUpdating 3rd-party bundle binaries...\n", "update_binaries", third_party_update_wrapper_script);
}

static enum swupd_code update_repos(UNUSED_PARAM char *unused)
{
	/* Update should always ignore optional bundles */
	globals.skip_optional_bundles = true;
	globals.no_scripts = true;

	if (cmdline_option_status) {
		return check_update();
	} else {
		info("Updates from a 3rd-party repository are forced to run with the --no-scripts flag for security reasons\n\n");
		return execute_update_extra(update_exported_binaries, validate_file_permissions);
	}
}

enum swupd_code third_party_execute_update(void)
{
	enum swupd_code ret_code = SWUPD_OK;
	struct list *repos = NULL;
	char *template_file = NULL;
	char *template = NULL;
	size_t template_len;
	int steps_in_update;
	int ret;

	/* 3rd-party updates can be executed also from the update command
	 * using the --3rd-party option, make sure a flag was not set in that
	 * command before calculating steps */
	cmdline_option_download_only |= update_get_option_download_only();

	/* the --update-search-file-index is not supported for 3rd-party
	 * so set it to false in case it was set up to true by update */
	update_set_option_update_search_file_index(false);

	/*
	 * Steps for update:
	 *   1) load_manifests
	 *   2) run_preupdate_scripts
	 *   3) download_packs
	 *   4) extract_packs
	 *   5) prepare_for_update
	 *   6) validate_fullfiles
	 *   7) download_fullfiles
	 *   8) extract_fullfiles (finishes here on --download)
	 *   9) update_files
	 *   10) update_binaries
	 *   11) run_postupdate_scripts
	 */
	if (cmdline_option_status) {
		steps_in_update = 0;
	} else if (cmdline_option_download_only) {
		steps_in_update = 8;
	} else {
		steps_in_update = 11;
	}

	/* update 3rd-party bundles */
	ret_code = third_party_run_operation_multirepo(cmdline_option_repo, update_repos, SWUPD_NO, "update", steps_in_update);
	if (ret_code) {
		goto exit;
	}

	/* if there are no 3rd-party repos we are done */
	repos = third_party_get_repos();
	if (!repos) {
		goto exit;
	}

	/* read the current template copy */
	template_file = sys_path_join("%s/%s/%s", globals_bkp.path_prefix, SWUPD_3RD_PARTY_DIR, SWUPD_3RD_PARTY_TEMPLATE_FILE);
	template = sys_mmap_file(template_file, &template_len);

	if (!template || str_cmp(template, SCRIPT_TEMPLATE) != 0) {
		/* there is no template file, or the template changed,
		 * all scripts need to be recreated */
		info("The scripts that export binaries from 3rd-party repositories need to be regenerated\n\n");
		ret_code = third_party_run_operation_multirepo(NULL, regenerate_all_wrapper_scripts, SWUPD_OK, "regenerate_scripts", steps_in_update);

		/* update the template */

		ret = sys_write_file(template_file, SCRIPT_TEMPLATE, str_len(SCRIPT_TEMPLATE));
		if (ret < 0) {
			error("The wrapper scripts template file %s failed to be updated\n", template_file);
			ret_code = SWUPD_COULDNT_WRITE_FILE;
		}
	}

exit:
	list_free_list_and_data(repos, repo_free_data);
	FREE(template_file);
	sys_mmap_free(template, template_len);

	return ret_code;
}

enum swupd_code third_party_update_main(int argc, char **argv)
{
	enum swupd_code ret_code = SWUPD_OK;

	if (!parse_options(argc, argv)) {
		print("\n");
		print_help();
		return SWUPD_INVALID_OPTION;
	}

	ret_code = swupd_init(SWUPD_ALL);
	if (ret_code != SWUPD_OK) {
		error("Failed swupd initialization, exiting now\n");
		return ret_code;
	}

	/* set the command options */
	update_set_option_version(cmdline_option_version);
	update_set_option_download_only(cmdline_option_download_only);
	update_set_option_keepcache(cmdline_option_keepcache);

	ret_code = third_party_execute_update();

	swupd_deinit();

	return ret_code;
}

#endif
