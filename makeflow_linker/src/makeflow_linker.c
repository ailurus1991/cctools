/*
Copyright (C) 2013- The University of Notre Dame
This software is distributed under the GNU General Public License.
See the file COPYING for details.
*/

#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <limits.h>
#include <string.h>
#include <time.h>

#include "cctools.h"
#include "copy_stream.h"
#include "create_dir.h"
#include "delete_dir.h"
#include "debug.h"
#include "getopt_aux.h"
#include "list.h"
#include "path.h"
#include "stringtools.h"
#include "xxmalloc.h"

#define MAKEFLOW_PATH "makeflow"
#define MAKEFLOW_BUNDLE_FLAG "-b"

typedef enum {UNKNOWN, EXPLICIT, MAKEFLOW, PERL, PYTHON} file_type;
static const char *file_type_strings[] = {"Unknown", "Explicit", "Makeflow", "Perl", "Python", NULL};
static const char *file_type_to_string(int type){
	if(type >= 0 && type < 5){
		return file_type_strings[type];
	}
	else {
		return "";
	}
}

enum { LONG_OPT_DRY_RUN = 1,
       LONG_OPT_VERBOSE,
     };

struct dependency{
	char *original_name;
	char *final_name;
	struct dependency *parent;
	struct dependency *superparent;
	char *output_path;
	int  depth;
	int searched;
	file_type type;
};

static int use_explicit = 0;
static int dry_run = 0;
static int verbose = 0;
static char *workspace = NULL;

char *python_extensions[2]   = { "py", "pyc" };
char *perl_extensions[2]     = { "pl", "pm" };
char *makeflow_extensions[2] = { "mf", "makeflow" };

void create_workspace(){
	workspace = (char *) malloc(PATH_MAX * sizeof(char));
	if(dry_run){
		workspace = xxstrdup("*");
	} else {
		snprintf(workspace, PATH_MAX, "/tmp/makeflow_linker_workspace_%d", rand()%2718 + 1);
		if(!create_dir(workspace, 0777)) fatal("Could not create directory.\n");
	}

	if(verbose) fprintf(stdout, "Created temporary workspace: %s\n", workspace);
}

void display_dependencies(struct list *d, int verbose){
	struct dependency *dep;

	list_first_item(d);
	while((dep = list_next_item(d))){
		if(dep->type != EXPLICIT){
			if(verbose) {
				if(dep->parent){
					printf("%s %s %d %d %s %s %s\n", dep->original_name, dep->final_name, dep->depth, dep->type, dep->parent->final_name, dep->superparent->final_name, dep->output_path);
				} else {
					printf("%s %s %d %d n/a n/a %s\n", dep->original_name, dep->final_name, dep->depth, dep->type, dep->output_path);
				}
			} else {
				printf("%s -> %s\n", dep->original_name, dep->output_path);
			}
		}
	}
}

file_type file_extension_known(const char *filename){
	const char *extension = path_extension(filename);

	if(!extension) extension = "";

	int j;
	for(j=0; j< 2; j++){
		if(!strcmp(python_extensions[j], extension))
			return PYTHON;
		if(!strcmp(perl_extensions[j], extension))
			return PERL;
		if(!strcmp(makeflow_extensions[j], extension))
			return MAKEFLOW;
	}

	return UNKNOWN;
}

file_type find_driver_for(const char *name){
	file_type type = UNKNOWN;

	if((type = file_extension_known(name))){
		if(verbose) fprintf(stdout, "%s is a %s file.\n", name, file_type_to_string(type));
	} else {
		if(verbose) fprintf(stdout, "%s is an Unknown file.\n", name);
	}

	return type;
}

void initialize( char *output_directory, char *input_file, struct list *d){
	srand(time(NULL));
	create_workspace();

	char expanded_input[PATH_MAX];
	realpath(input_file, expanded_input);

	struct dependency *initial_dependency = (struct dependency *) malloc(sizeof(struct dependency));
	initial_dependency->original_name = xxstrdup(expanded_input);
	initial_dependency->final_name = xxstrdup(path_basename(expanded_input));
	initial_dependency->type = find_driver_for(expanded_input);
	initial_dependency->depth = 0;
	initial_dependency->parent = NULL;
	initial_dependency->superparent = NULL;
	initial_dependency->searched = 0;
	list_push_tail(d, (void *) initial_dependency);
}

struct list *find_dependencies_for(struct dependency *dep){
	pid_t pid;
	int pipefd[2];
	pipe(pipefd);

	if(dep->type == EXPLICIT){
		struct list *empty = list_create();
		return empty;
	}

	switch ( pid = fork() ){
	case -1:
		fprintf( stderr, "Cannot fork. Exiting...\n" );
		exit(1);
	case 0:
		/* Child process */
		close(pipefd[0]);
		dup2(pipefd[1], 1);
		close(pipefd[1]);
		char *args[] = { "locating dependencies" , NULL, NULL, NULL, NULL };
		if(use_explicit){
			args[1] = "--use-explicit";
			args[2] = dep->original_name;
		} else {
			args[1] = dep->original_name;
		}
		switch ( dep->type ){
			case PERL:
				execvp("perl_driver", args);
			case PYTHON:
				execvp("python_driver", args);
			case EXPLICIT:
				break;
			case MAKEFLOW:
				args[1] = MAKEFLOW_BUNDLE_FLAG;
				args[2] = workspace;
				args[3] = dep->original_name;
				execvp(MAKEFLOW_PATH, args);
				break;
			case UNKNOWN:
				break;
		}
		exit(1);
	default:
		/* Parent process */
		close(pipefd[1]);
		char next;
		char *buffer = (char *) malloc(sizeof(char));
		char *original_name;
		int size = 0;
		int depth = dep->depth  + 1;
		struct list *new_deps = list_create();
		int explicit = 0;

		while (read(pipefd[0], &next, sizeof(next)) != 0){
			switch ( next ){
				case '*':
					explicit = 1;
					break;
				case '\t':
				case ' ':
					if(explicit){
						buffer = realloc(buffer, size+1);
						*(buffer+size) = next;
						size++;
					} else {
						original_name = (char *)realloc((void *)buffer,size+1);
						*(original_name+size) = '\0';
						buffer = (char *) malloc(sizeof(char));
						size = 0;
					}
					break;
				case '\n':
					buffer = realloc(buffer, size+1);
					*(buffer+size) = '\0';
					struct dependency *new_dependency = (struct dependency *) malloc(sizeof(struct dependency));
					if(explicit){
						new_dependency->original_name = buffer;
						new_dependency->final_name = buffer;
						new_dependency->type = EXPLICIT;
					} else {
						new_dependency->original_name = original_name;
						new_dependency->final_name = buffer;
					}
					new_dependency->depth = depth;
					new_dependency->parent = dep;
					if(dep->superparent){
						new_dependency->superparent = dep->superparent;
					} else {
						new_dependency->superparent = dep;
					}
					list_push_tail(new_deps, new_dependency);
					size = 0;
					buffer = NULL;
					explicit = 0;
					break;
				default:
					buffer = realloc(buffer, size+1);
					*(buffer+size) = next;
					size++;
			}
		}
		dep->searched = 1;
		return new_deps;
	}
}

void find_dependencies(struct list *d){
	struct dependency *dep;
	struct list *new;

	list_first_item(d);
	do {
		dep = list_peek_current(d);
		if(!dep) break;

		if(dep->searched) continue;
		new = find_dependencies_for(dep);
		list_first_item(new);
		while((dep = list_next_item(new))){
			if(dep->type != EXPLICIT)
				dep->type = find_driver_for(dep->original_name);
			list_push_tail(d, dep);
		}
		list_delete(new);
		new = NULL;
	}while(list_next_item(d));
}

void find_drivers(struct list *d){
	struct dependency *dep;
	list_first_item(d);
	while((dep = list_next_item(d))){
		dep->type = find_driver_for(dep->original_name);
	}
}

void determine_package_structure(struct list *d, char *output_dir){
	struct dependency *dep;
	list_first_item(d);
	while((dep = list_next_item(d))){
		char resolved_path[PATH_MAX];
		if(dep->parent && dep->parent->type != MAKEFLOW && dep->parent->output_path){
			sprintf(resolved_path, "%s", dep->parent->output_path);
		} else {
			sprintf(resolved_path, "%s", output_dir);
		}
		switch(dep->type){
			case PYTHON:
				sprintf(resolved_path, "%s/%s", resolved_path, dep->final_name);
				break;
			case MAKEFLOW:
				sprintf(resolved_path, "%s/%s", resolved_path, dep->final_name);
				break;
			case PERL:
			default:
				/* TODO: naming conflicts */
				break;
		}
		dep->output_path = xxstrdup(resolved_path);
	}
}

void build_package(struct list *d){
	struct dependency *dep;
	list_first_item(d);
	while((dep = list_next_item(d))){
		char tmp_from_path[PATH_MAX];
		char tmp_dest_path[PATH_MAX];
		switch(dep->type){
			case PYTHON:
				if(!create_dir(dep->output_path, 0777)) fatal("Could not create directory.\n");
				if(dep->depth > 1)
					sprintf(tmp_dest_path, "%s/__init__.py", dep->output_path);
				else
					sprintf(tmp_dest_path, "%s/__main__.py", dep->output_path);
				copy_file_to_file(dep->original_name, tmp_dest_path);
				break;
			case MAKEFLOW:
				sprintf(tmp_from_path, "%s/%s", workspace, dep->final_name);
				copy_file_to_file(tmp_from_path, dep->output_path);
				break;
			case PERL:
			default:
				sprintf(tmp_dest_path, "%s/%s", dep->output_path, dep->final_name);
				copy_file_to_file(dep->original_name, tmp_dest_path);
				break;
		}
	}
}

struct list *list_explicit(struct list *d){
	struct dependency *dep;
	struct list *explicit_dependencies = list_create();

	list_first_item(d);
	while((dep = list_next_item(d))){
		if(dep->type == EXPLICIT){
			if(!list_find(explicit_dependencies, (int (*)(void *, const void *)) string_equal, (void *) dep->original_name))
				list_push_tail(explicit_dependencies, dep->original_name);
		}
	}

	return explicit_dependencies;
}

void write_explicit(struct list *l, const char *output){
	char *path = string_format("%s/explicit", output);
	FILE *fp = NULL;
	char *dep;
	if(list_size(l) > 0){
		fp = fopen(path, "w");
		list_first_item(l);
		while((dep = list_next_item(l))){
			fprintf(fp, "%s\n", dep);
		}
		fclose(fp);
	}
	free(path);
}

static void show_help(const char *cmd){
	fprintf(stdout, "Use: %s [options] <workflow_description>\n", cmd);
	fprintf(stdout, "Frequently used options:\n");
	fprintf(stdout, "%-30s Do not copy files which are part of an explicit dependency, e.g. standard libraries\n", "-e, --use-explicit");
	fprintf(stdout, "%-30s Show this help screen.\n", "-h,--help");
	fprintf(stdout, "%-30s Specify output directory, default:output_dir\n", "-o,--output");
}

void cleanup(){
	if(workspace){
		if(delete_dir(workspace) != 0) fprintf(stderr, "Could not delete workspace (%s)\n", workspace);
		if(verbose) fprintf(stdout, "Deleted temporary workspace: %s\n", workspace);
		free(workspace);
	}
}

int main(int argc, char *argv[]){
	char *output = NULL;
	char *input  = NULL;

	int c;

	struct option long_options[] = {
		{"use-explicit", no_argument, 0, 'e'},
		{"dry-run", no_argument, 0, LONG_OPT_DRY_RUN},
		{"help", no_argument, 0, 'h'},
		{"output", required_argument, 0, 'o'},
		{"verbose", no_argument, 0, LONG_OPT_VERBOSE},
		{"version", no_argument, 0, 'v'},
		{0, 0, 0, 0}
	};

	while((c = getopt_long(argc, argv, "eho:v", long_options, NULL)) >= 0){
		switch(c){
			case 'e':
				use_explicit = 1;
				break;
			case LONG_OPT_DRY_RUN:
				dry_run = 1;
				break;
			case 'o':
				output = xxstrdup(optarg);
				break;
			case 'h':
				show_help(argv[0]);
				return 0;
			case 'v':
				cctools_version_print(stdout, argv[0]);
				return 0;
			case LONG_OPT_VERBOSE:
				verbose = 1;
				break;
			default:
				show_help(argv[0]);
				return 1;
		}
	}

	if(!output) output = xxstrdup("output_dir");
	if((argc - optind) != 1)
		fatal("makeflow_linker: No workflow description specified.\n");
	
	input = argv[optind];

	struct list *dependencies;
	dependencies = list_create();

	initialize(output, input, dependencies);
	char *tmp = output;
	output = (char *) malloc(PATH_MAX * sizeof(char));
	realpath(tmp, output);
	free(tmp);
	if(!create_dir(output, 0777)) fatal("Could not create output directory.\n");

	char input_wd[PATH_MAX];
	path_dirname(input, input_wd);
	chdir(input_wd); 

	find_drivers(dependencies);
	find_dependencies(dependencies);

	determine_package_structure(dependencies, output);
	if(!dry_run) build_package(dependencies);

	struct list *l = list_explicit(dependencies);
	if(!dry_run) write_explicit(l, output);

	if(dry_run) display_dependencies(dependencies, 0);

	cleanup();

	return 0;
}
