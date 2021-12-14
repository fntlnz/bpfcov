#define _GNU_SOURCE

#ifdef NDEBUG
#define DEBUG 0
#else
#define DEBUG 1
#endif

/* C standard library */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <time.h>
#include <string.h>

/* POSIX */
#include <unistd.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/vfs.h>

/* Linux */
#include <syscall.h>
#include <sys/ptrace.h>
#include <linux/limits.h>
#include <linux/magic.h>
#include <bpf/bpf.h>

#include <argp.h>

// --------------------------------------------------------------------------------------------------------------------
// Global info
// --------------------------------------------------------------------------------------------------------------------

#define TOOL_NAME "bpfcov"
const char *argp_program_version = TOOL_NAME " 0.1";
const char *argp_program_bug_address = "leo";
error_t argp_err_exit_status = 1;

// --------------------------------------------------------------------------------------------------------------------
// Prototypes
// --------------------------------------------------------------------------------------------------------------------

const char *argp_key(int key, char *str);

struct root_args;
typedef int (*callback_t)(struct root_args *args);

static error_t root_parse(int key, char *arg, struct argp_state *state);
void root(int argc, char **argv);

void run_cmd(struct argp_state *state);
static error_t run_parse(int key, char *arg, struct argp_state *state);
int run(struct root_args *args);

void gen_cmd(struct argp_state *state);
static error_t gen_parse(int key, char *arg, struct argp_state *state);
int gen(struct root_args *args);

static bool is_bpffs(char *bpffs_path);
static void strip_trailing_char(char *str, char c);
static void replace_with(char *str, const char what, const char with);

// --------------------------------------------------------------------------------------------------------------------
// Logging
// --------------------------------------------------------------------------------------------------------------------

void print_log(int level, const char *prefix, struct root_args *args, const char *fmt, ...);

#define log_erro(args, fmt, ...)                                      \
    do                                                                \
    {                                                                 \
        if (DEBUG)                                                    \
            print_log(0, TOOL_NAME ": %s: ", args, fmt, __VA_ARGS__); \
    } while (0)

#define log_warn(args, fmt, ...)                                      \
    do                                                                \
    {                                                                 \
        if (DEBUG)                                                    \
            print_log(1, TOOL_NAME ": %s: ", args, fmt, __VA_ARGS__); \
    } while (0)

#define log_info(args, fmt, ...)                                      \
    do                                                                \
    {                                                                 \
        if (DEBUG)                                                    \
            print_log(2, TOOL_NAME ": %s: ", args, fmt, __VA_ARGS__); \
    } while (0)

#define log_debu(args, fmt, ...)                                      \
    do                                                                \
    {                                                                 \
        if (DEBUG)                                                    \
            print_log(3, TOOL_NAME ": %s: ", args, fmt, __VA_ARGS__); \
    } while (0)

#define log_fata(args, fmt, ...)      \
    log_erro(args, fmt, __VA_ARGS__); \
    exit(EXIT_FAILURE);

// --------------------------------------------------------------------------------------------------------------------
// Entrypoint
// --------------------------------------------------------------------------------------------------------------------

int main(int argc, char **argv)
{
    root(argc, argv);

    return 0;
}

// --------------------------------------------------------------------------------------------------------------------
// CLI / bpfcov
// --------------------------------------------------------------------------------------------------------------------

#define NUM_PINNED_MAPS 4

struct root_args
{
    char *output;
    char *bpffs;
    char *cov_root;
    char *prog_root;
    char *pin[NUM_PINNED_MAPS];
    int verbosity;
    callback_t command;
    char **program;
};

const char ROOT_BPFFS_OPT_KEY = 0x80;
const char ROOT_BPFFS_OPT_LONG[] = "bpffs";
const char ROOT_BPFFS_OPT_ARG[] = "path";
const char ROOT_VERBOSITY_OPT_KEY = 'v';
const char ROOT_VERBOSITY_OPT_LONG[] = "verbose";
const char ROOT_VERBOSITY_OPT_ARG[] = "level";

static struct argp_option root_opts[] = {
    {"OPTIONS:", 0, 0, OPTION_DOC, 0, 0},
    {ROOT_BPFFS_OPT_LONG, ROOT_BPFFS_OPT_KEY, ROOT_BPFFS_OPT_ARG, 0, "Set the BPF FS path (defaults to /sys/fs/bpf)", 1},
    {

        ROOT_VERBOSITY_OPT_LONG,
        ROOT_VERBOSITY_OPT_KEY,
        ROOT_VERBOSITY_OPT_ARG,
        OPTION_ARG_OPTIONAL,
        "Set the verbosity level when not built for release (defaults to 0)",
        -1

    },
    {0} // .
};

static char root_docs[] =
    "\n"
    "Obtain coverage from your instrumented eBPF programs."
    "\v"
    "  EXAMPLES:\n"
    "  bpfcov run <program>\n"
    "  bpfcov gen <program>\n"
    "  bpfcov <program>\n";

static struct argp root_argp = {
    .options = root_opts,
    .parser = root_parse,
    .args_doc = "[run|gen] <program>",
    .doc = root_docs,
};

static error_t root_parse(int key, char *arg, struct argp_state *state)
{
    struct root_args *args = state->input;

    char str[2];
    log_debu(args, "parsing <root> %s = '%s'\n", argp_key(key, str), arg ? arg : "(null)");

    switch (key)
    {

    // Initialization
    case ARGP_KEY_INIT:
        args->bpffs = "/sys/fs/bpf";
        // args->verbosity = 0; // It needs to be set before the parsing starts
        args->command = NULL;
        args->program = calloc(PATH_MAX, sizeof(char *));
        break;

    case ROOT_BPFFS_OPT_KEY:
        if (strlen(arg) > 0)
        {
            strip_trailing_char(arg, '/');
            args->bpffs = arg;
            break;
        }
        argp_error(state, "option '--%s' requires a %s", ROOT_BPFFS_OPT_LONG, ROOT_BPFFS_OPT_ARG);
        break;

    case ROOT_VERBOSITY_OPT_KEY:
        if (arg)
        {
            errno = 0;
            char *end;
            long num = strtol(arg, &end, 10);
            if (end == arg)
            {
                argp_error(state, "option '--%s' requires a numeric %s", ROOT_VERBOSITY_OPT_LONG, ROOT_VERBOSITY_OPT_ARG);
            }
            if (num < 0 || num > 3)
            {
                argp_error(state, "option '--%s' requires a %s value in [0,3]", ROOT_VERBOSITY_OPT_LONG, ROOT_VERBOSITY_OPT_ARG);
            }

            args->verbosity = (int)num;
        }
        else
        {
            args->verbosity++;
        }
        break;

    case ARGP_KEY_ARG:
        assert(arg);

        if (strncmp(arg, "run", 3) == 0)
        {
            args->command = &run;
            run_cmd(state);
        }
        else if (strncmp(arg, "gen", 3) == 0)
        {
            args->command = &gen;
            gen_cmd(state);
        }
        else
        {
            args->program[state->arg_num] = arg;
        }

        break;

    // Args validation
    case ARGP_KEY_END:
        if (state->arg_num == 0)
        {
            argp_state_help(state, state->err_stream, ARGP_HELP_STD_HELP);
        }
        if (args->program[0] == NULL)
        {
            // This should never happen
            argp_error(state, "unexpected missing <program>");
        }
        // TODO(leodido) > check it is an eBPF ELF
        // TODO(leodido) > check it contains bpfcov instrumentation
        break;

    // Final validations, checks, and settings
    case ARGP_KEY_FINI:
        bool is_gen = args->command == &gen;
        bool is_run = args->command == &run;

        // Check the BPF filesystem
        if (!is_bpffs(args->bpffs))
        {
            argp_error(state, "the BPF filesystem is not mounted at %s", args->bpffs);
        }

        // Create the coverage directory in the BPF filesystem
        char cov_root[PATH_MAX];
        int cov_root_len = snprintf(cov_root, PATH_MAX, "%s/%s", args->bpffs, "cov");
        if (cov_root_len >= PATH_MAX)
        {
            argp_error(state, "coverage root path too long");
        }
        if (is_run && mkdir(cov_root, 0700) && errno != EEXIST)
        {
            argp_error(state, "could not create '%s'", cov_root);
        }
        args->cov_root = cov_root;

        // Obtain the program name and create a directory in the BPF filesystem for it
        char *prog_name = basename(args->program[0]);
        char prog_root[PATH_MAX];
        int prog_root_len = snprintf(prog_root, PATH_MAX, "%s/%s", cov_root, prog_name);
        if (prog_root_len >= PATH_MAX)
        {
            argp_error(state, "program root path too long");
        }
        char *prog_root_sane = strdup(prog_root);
        replace_with(prog_root_sane, '.', '_'); // Sanitize because BPF FS doesn't accept dots
        if (is_run && mkdir(prog_root_sane, 0700) && errno != EEXIST)
        {
            argp_error(state, "could not create '%s'", prog_root_sane);
        }
        args->prog_root = prog_root_sane;
        log_info(args, "root directory for map pins at '%s'\n", prog_root_sane);

        // Create pinning path for the counters map
        char pin_profc[PATH_MAX];
        int pin_profc_len = snprintf(pin_profc, PATH_MAX, "%s/%s", prog_root_sane, "profc");
        if (pin_profc_len >= PATH_MAX)
        {
            argp_error(state, "counters pinning path too long");
        }
        args->pin[0] = pin_profc;

        // Create pinning path for the data map
        char pin_profd[PATH_MAX];
        int pin_profd_len = snprintf(pin_profd, PATH_MAX, "%s/%s", prog_root_sane, "profd");
        if (pin_profd_len >= PATH_MAX)
        {
            argp_error(state, "data pinning path too long");
        }
        args->pin[1] = pin_profd;

        // Create pinning path for the names map
        char pin_profn[PATH_MAX];
        int pin_profn_len = snprintf(pin_profn, PATH_MAX, "%s/%s", prog_root_sane, "profn");
        if (pin_profn_len >= PATH_MAX)
        {
            argp_error(state, "names pinning path too long");
        }
        args->pin[2] = pin_profn;

        // Create pinning path for the coverage mapping header
        char pin_covmap[PATH_MAX];
        int pin_covmap_len = snprintf(pin_covmap, PATH_MAX, "%s/%s", prog_root_sane, "covmap");
        if (pin_covmap_len >= PATH_MAX)
        {
            argp_error(state, "coverage mapping header path too long");
        }
        args->pin[3] = pin_covmap;

        // Check whether the map pinning paths already exist
        int p;
        for (p = 0; p < NUM_PINNED_MAPS; p++)
        {
            if (access(args->pin[p], F_OK) == 0)
            {
                // Unpin them in case they do exist and the current subcommand is `run`
                if (is_run)
                {
                    log_warn(args, "unpinning existing map '%s'\n", args->pin[p]);
                    if (unlink(args->pin[p]) != 0)
                    {
                        argp_error(state, "could not unpin map '%s'", args->pin[p]);
                    }
                }
            }
            else
            {
                // Error out in case the do not exist and the current subcommand is `gen`
                if (is_gen)
                {
                    argp_error(state, "could not access map '%s'", args->pin[p]);
                }
            }
        }

        break;

    default:
        log_debu(args, "parsing <root> UNKNOWN = '%s'\n", arg ? arg : "(null)");
        return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

void root(int argc, char **argv)
{
    struct root_args this = {
        .verbosity = 0,
    };
    argp_parse(&root_argp, argc, argv, ARGP_IN_ORDER, NULL, &this);

    if (this.command)
    {
        this.command(&this);
    }
    else
    {
        fprintf(stderr, "TBD\n");
        // run(&this);
        // gen(&this);
    }
}

// --------------------------------------------------------------------------------------------------------------------
// CLI / bpfcov run
// --------------------------------------------------------------------------------------------------------------------

struct run_args
{
    struct root_args *parent;
};

static struct argp_option run_opts[] = {
    {0} // .
};

static char run_docs[] = "\n"
                         "Execute your bpfcov instrumented program.\n"
                         "\n";

static struct argp run_argp = {
    .options = run_opts,
    .parser = run_parse,
    .args_doc = "<program>",
    .doc = run_docs,
};

static error_t
run_parse(int key, char *arg, struct argp_state *state)
{
    struct run_args *args = state->input;

    assert(args);
    assert(args->parent);

    char str[2];
    log_debu(args->parent, "parsing <run> %s = '%s'\n", argp_key(key, str), arg ? arg : "(null)");

    switch (key)
    {
    case ARGP_KEY_ARG:
        args->parent->program[state->arg_num] = arg;
        break;

    case ARGP_KEY_END:
        if (!args->parent->program[0])
        {
            argp_error(state, "missing program argument");
        }
        if (access(args->parent->program[0], F_OK) != 0)
        {
            argp_error(state, "program '%s' does not actually exist", args->parent->program[0]);
        }
        break;

    default:
        log_debu(args->parent, "parsing <run> UNKNOWN = '%s'\n", arg ? arg : "(null)");
        return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

void run_cmd(struct argp_state *state)
{
    struct run_args args = {};
    int argc = state->argc - state->next + 1;
    char **argv = &state->argv[state->next - 1];
    char *argv0 = argv[0];

    args.parent = state->input;

    log_debu(args.parent, "begin <run> (argc = %d, argv[0] = %s)\n", argc, argv[0]);

    argv[0] = malloc(strlen(state->name) + strlen(" run") + 1);
    if (!argv[0])
    {
        argp_failure(state, 1, ENOMEM, 0);
    }
    sprintf(argv[0], "%s run", state->name);

    argp_parse(&run_argp, argc, argv, ARGP_IN_ORDER, &argc, &args);

    free(argv[0]);

    argv[0] = argv0;

    state->next += argc - 1;

    log_debu(args.parent, "end <run> (next = %d, argv[next] = %s)\n", state->next, state->argv[state->next]);
}

// --------------------------------------------------------------------------------------------------------------------
// CLI / bpfcov gen
// --------------------------------------------------------------------------------------------------------------------

struct gen_args
{
    struct root_args *parent;
};

const char GEN_OUTPUT_OPT_KEY = 'o';
const char GEN_OUTPUT_OPT_LONG[] = "output";
const char GEN_OUTPUT_OPT_ARG[] = "path";

static struct argp_option gen_opts[] = {
    {"OPTIONS:", 0, 0, OPTION_DOC, 0, 0},
    {GEN_OUTPUT_OPT_LONG, GEN_OUTPUT_OPT_KEY, GEN_OUTPUT_OPT_ARG, 0, "Set the output path\n(defaults to <program>.profraw)", 1},
    {0} // .
};

static char gen_docs[] = "\n"
                         "Generate the profraw file for the bpfcov instrumented program.\n"
                         "\n";

static struct argp gen_argp = {
    .options = gen_opts,
    .parser = gen_parse,
    .args_doc = "<program>",
    .doc = gen_docs,
};

static error_t
gen_parse(int key, char *arg, struct argp_state *state)
{
    struct gen_args *args = state->input;

    assert(args);
    assert(args->parent);

    char str[2];
    log_debu(args->parent, "parsing <gen> %s = '%s'\n", argp_key(key, str), arg ? arg : "(null)");

    switch (key)
    {
    case GEN_OUTPUT_OPT_KEY:
        if (strlen(arg) > 0)
        {
            args->parent->output = arg;
            break;
        }
        argp_error(state, "option '--%s' requires a %s", GEN_OUTPUT_OPT_LONG, GEN_OUTPUT_OPT_ARG);
        break;

    case ARGP_KEY_ARG:
        // NOTE > Collecting also other arguments/options even though they are not used to generate the pinning path
        args->parent->program[state->arg_num] = arg;
        break;

    case ARGP_KEY_END:
        if (!args->parent->program[0])
        {
            argp_error(state, "missing program argument");
        }
        if (access(args->parent->program[0], F_OK) != 0)
        {
            argp_error(state, "program '%s' does not actually exist", args->parent->program[0]);
        }
        if (!args->parent->output)
        {
            char output_path[PATH_MAX];
            int output_path_len = snprintf(output_path, PATH_MAX, "%s.%s", args->parent->program[0], "profraw");
            if (output_path_len >= PATH_MAX)
            {
                argp_error(state, "output path too long");
            }
            args->parent->output = output_path;
        }
        break;

    default:
        log_debu(args->parent, "parsing <gen> UNKNOWN = '%s'\n", arg ? arg : "(null)");
        return ARGP_ERR_UNKNOWN;
    }

    return 0;
}

void gen_cmd(struct argp_state *state)
{
    struct gen_args args = {};
    int argc = state->argc - state->next + 1;
    char **argv = &state->argv[state->next - 1];
    char *argv0 = argv[0];

    args.parent = state->input;

    log_debu(args.parent, "begin <gen> (argc = %d, argv[0] = %s)\n", argc, argv[0]);

    argv[0] = malloc(strlen(state->name) + strlen(" gen") + 1);
    if (!argv[0])
    {
        argp_failure(state, 1, ENOMEM, 0);
    }
    sprintf(argv[0], "%s gen", state->name);

    argp_parse(&gen_argp, argc, argv, ARGP_IN_ORDER, &argc, &args);

    free(argv[0]);

    argv[0] = argv0;

    state->next += argc - 1;

    log_debu(args.parent, "end <gen> (next = %d, argv[next] = %s)\n", state->next, state->argv[state->next]);
}

// --------------------------------------------------------------------------------------------------------------------
// Miscellaneous
// --------------------------------------------------------------------------------------------------------------------

const char *argp_key(int key, char *str)
{
    str[0] = key;
    str[1] = 0;

    switch (key)
    {
    case ARGP_KEY_ARG:
        return "ARGP_KEY_ARG";
    case ARGP_KEY_ARGS:
        return "ARGP_KEY_ARGS";
    case ARGP_KEY_END:
        return "ARGP_KEY_END";
    case ARGP_KEY_NO_ARGS:
        return "ARGP_KEY_NO_ARGS";
    case ARGP_KEY_INIT:
        return "ARGP_KEY_INIT";
    case ARGP_KEY_SUCCESS:
        return "ARGP_KEY_SUCCESS";
    case ARGP_KEY_ERROR:
        return "ARGP_KEY_ERROR";
    case ARGP_KEY_FINI:
        return "ARGP_KEY_FINI";
    }

    return str;
};

void print_log(int level, const char *prefix, struct root_args *args, const char *fmt, ...)
{
    if (args->verbosity < level)
    {
        return;
    }

    FILE *f = level == 0 ? stderr : stdout;
    va_list argptr;
    va_start(argptr, fmt);

    if (!prefix || (prefix && !*prefix))
    {
        goto without_prefix;
    }

    char *qual = "unkn";
    switch (level)
    {
    case 0:
        qual = "erro";
        break;
    case 1:
        qual = "warn";
        break;
    case 2:
        qual = "info";
        break;
    case 3:
        qual = "debu";
        break;
    }

    fprintf(f, "bpfcov: %s: ", qual);

without_prefix:
    vfprintf(f, fmt, argptr);
    va_end(argptr);
}

static bool is_bpffs(char *bpffs_path)
{
    struct statfs st_fs;

    if (statfs(bpffs_path, &st_fs) < 0)
        return false;

    return st_fs.f_type == BPF_FS_MAGIC;
}

static void strip_trailing_char(char *str, char c)
{
    int last = strlen(str) - 1;
    while (last > 0 && str[last] == c)
    {
        str[last--] = '\0';
    }
}

static void replace_with(char *str, const char what, const char with)
{
    while (*str)
    {
        if (*str == what)
        {
            *str = with;
        }
        str++;
    }
}

static int get_pin_path(struct root_args *args, char *suffix, char **pin_path)
{
    if (strncmp(suffix, "profc", 5) == 0)
    {
        *pin_path = args->pin[0];
        return 1;
    }
    else if (strncmp(suffix, "profd", 5) == 0)
    {
        *pin_path = args->pin[1];
        return 1;
    }
    else if (strncmp(suffix, "profn", 5) == 0)
    {
        *pin_path = args->pin[2];
        return 1;
    }
    else if (strncmp(suffix, "covmap", 6) == 0)
    {
        *pin_path = args->pin[3];
        return 1;
    }

    return 0;
}

static int get_map_info(int fd, struct bpf_map_info *info)
{
    unsigned int size = sizeof(*info);
    memset(info, 0, size);
    return bpf_obj_get_info_by_fd(fd, info, &size);
    // TODO(leodido) ? close fd if above fails?
}

static int get_global_data(int fd, struct bpf_map_info *info, void *data)
{
    int err;
    void *k, *v;
    k = malloc(info->key_size);
    v = malloc(info->value_size);
    if (!k || !v)
    {
        err = -1;
        goto error_out;
    }
    if (!info)
    {
        err = -1;
        goto error_out;
    }
    if (info->max_entries > 1)
    {
        err = -1;
        goto error_out;
    }

    err = bpf_map_get_next_key(fd, NULL, k);
    if (err)
    {
        goto error_out;
    }
    err = bpf_map_lookup_elem(fd, k, v);
    if (err)
    {
        goto error_out;
    }
    memcpy(data, v, info->value_size);

error_out:
    free(k);
    free(v);
    close(fd);
    return err;
}

// --------------------------------------------------------------------------------------------------------------------
// Implementation
// --------------------------------------------------------------------------------------------------------------------

int run(struct root_args *args)
{
    log_info(args, "run: executing program '%s'\n", args->program[0]);

    pid_t pid = fork();
    switch (pid)
    {
    case -1: /* Error */
        log_fata(args, "run: %s\n", strerror(errno));
    case 0: /* Child */
        if (ptrace(PTRACE_TRACEME, 0, NULL, NULL) < 0)
        {
            log_fata(args, "%s\n", strerror(errno));
        }
        execvp(args->program[0], args->program);
        log_fata(args, "run: %s\n", strerror(errno));
    }

    /* Parent */
    waitpid(pid, 0, 0); // sync with PTRACE_TRACEME
    ptrace(PTRACE_SETOPTIONS, pid, 0, PTRACE_O_EXITKILL);

    int is_map = 0;
    for (;;)
    {
        /* Enter next system call */
        if (ptrace(PTRACE_SYSCALL, pid, 0, 0) == -1)
        {
            log_fata(args, "run: %s\n", strerror(errno));
        }
        if (waitpid(pid, 0, 0) == -1)
        {
            log_fata(args, "run: %s\n", strerror(errno));
        }

        /* Gather system call arguments */
        struct user_regs_struct regs;
        if (ptrace(PTRACE_GETREGS, pid, 0, &regs) == -1)
        {
            log_fata(args, "run: %s\n", strerror(errno));
        }

        /* Mark bpf(BPF_MAP_CREATE, ...) */
        const unsigned int sysc = regs.orig_rax;
        const unsigned int comm = regs.rdi;
        if (sysc == SYS_bpf && comm == BPF_MAP_CREATE)
        {
            is_map = 1;
        }

        /* Print a representation of the system call */
        log_debu(args,
                 "run: %d(%d, %ld, %ld, %ld, %ld, %ld)",
                 sysc,
                 comm, (long)regs.rsi, (long)regs.rdx, (long)regs.r10, (long)regs.r8, (long)regs.r9);

        /* Run system call and stop on exit */
        if (ptrace(PTRACE_SYSCALL, pid, 0, 0) == -1)
        {
            log_fata(args, "run: %s\n", strerror(errno));
        }
        if (waitpid(pid, 0, 0) == -1)
        {
            log_fata(args, "run: %s\n", strerror(errno));
        }

        /* Get system call result */
        if (ptrace(PTRACE_GETREGS, pid, 0, &regs) == -1)
        {
            print_log(3, NULL, args, "%s\n", " = ?");
            if (errno == ESRCH)
            {
                exit(regs.rdi); // _exit(2) or similar
            }
            log_fata(args, "run: %s\n", strerror(errno));
        }

        /* Print system call result */
        long result = regs.rax;
        print_log(3, NULL, args, " = %ld\n", result);

        /* Pin the bpfcov maps */
        if (is_map && result)
        {
            int pidfd = syscall(SYS_pidfd_open, pid, 0);
            if (pidfd < 0)
            {
                continue;
            }
            int curfd = syscall(SYS_pidfd_getfd, pidfd, result, 0);
            if (curfd < 0)
            {
                continue;
            }

            struct bpf_map_info map_info = {};
            memset(&map_info, 0, sizeof(map_info));
            unsigned int len = sizeof(map_info);

            int err;
            err = bpf_obj_get_info_by_fd(curfd, &map_info, &len);
            if (!err && strlen(map_info.name) > 0)
            {
                log_info(args, "run: got info about map '%s'\n", map_info.name);

                char map_name[BPF_OBJ_NAME_LEN];
                strcpy(map_name, map_info.name);

                const char *sep = ".";
                strtok(map_info.name, sep);
                char *suffix = strtok(NULL, sep);

                char *pin_path = "";
                if (get_pin_path(args, suffix, &pin_path))
                {
                    log_warn(args, "run: pinning map '%s' to '%s'\n", map_name, pin_path);
                    err = bpf_obj_pin(curfd, pin_path);
                }
                if (err)
                {
                    log_fata(args, "run: %s\n", "could not pin map");
                }
            }
        }
    }

    return 0;
}

int gen(struct root_args *args)
{
    log_info(args, "generating '%s' for program '%s'\n", args->output, args->program[0]);

    /* Get maps info */
    struct bpf_map_info profc_info = {};
    if (get_map_info(bpf_obj_get(args->pin[0]), &profc_info))
    {
        log_fata(args, "could not get info about pinned map '%s'\n", args->pin[0]);
    }
    struct bpf_map_info profd_info = {};
    if (get_map_info(bpf_obj_get(args->pin[1]), &profd_info))
    {
        log_fata(args, "could not get info about pinned map '%s'\n", args->pin[1]);
    }
    struct bpf_map_info profn_info = {};
    if (get_map_info(bpf_obj_get(args->pin[2]), &profn_info))
    {
        log_fata(args, "could not get info about pinned map '%s'\n", args->pin[2]);
    }
    struct bpf_map_info covmap_info = {};
    if (get_map_info(bpf_obj_get(args->pin[3]), &covmap_info))
    {
        log_fata(args, "could not get info about pinned map '%s'\n", args->pin[3]);
    }

    /* Time to write binary data to the output file */
    FILE *outfp = fopen(args->output, "wb");
    if (!outfp)
    {
        log_fata(args, "could not open the output file '%s'\n", args->output);
    }

    /* Write the header */
    log_info(args, "%s\n", "about to write the profraw header...");
    // Magic number
    char magic[8] = {0x81, 0x72, 0x66, 0x6F, 0x72, 0x70, 0x6C, 0xFF};
    fwrite(magic, 1, sizeof(magic), outfp);
    // Version
    void *covmap_data = malloc(covmap_info.value_size);
    if (get_global_data(bpf_obj_get(args->pin[3]), &covmap_info, covmap_data))
    {
        log_fata(args, "could not get global data from map '%s'\n", args->pin[3]);
    }
    long long int version = 0;
    memcpy(&version, &((char *)covmap_data)[12], 4); // Version is the 3rd int in the coverage mapping header
    version += 1;                                    // Version is 0 indexed
    fwrite(&version, 1, sizeof(version), outfp);
    free(covmap_data);
    // Data size
    long long int func_num = profd_info.value_size / 48; // 5 x i64 + 2 x i32 for each function
    fwrite(&func_num, 1, sizeof(func_num), outfp);
    // Padding before counters
    long long int pad_bef = 0;
    fwrite(&pad_bef, 1, sizeof(pad_bef), outfp);
    // Counters size
    long long int counters_num = profc_info.value_size / 8; // 1 x i64 for each counter element
    fwrite(&counters_num, 1, sizeof(counters_num), outfp);
    // Padding after counters
    long long int pad_aft = 0;
    fwrite(&pad_aft, 1, sizeof(pad_aft), outfp);
    // Names size
    fwrite(&profn_info.value_size, 1, sizeof(profn_info.value_size) * 2, outfp);
    // Counters delta (nulled)
    long long int counters_delta = 0;
    fwrite(&counters_delta, 1, sizeof(counters_delta), outfp);
    // Names delta (nulled)
    long long int names_delta = 0;
    fwrite(&names_delta, 1, sizeof(names_delta), outfp);
    // IPVK last
    long long int ipvk_last = 1;
    fwrite(&ipvk_last, 1, sizeof(ipvk_last), outfp);

    /* Write the data part */
    log_info(args, "%s\n", "about to write the data in the profraw...");
    void *profd_data = malloc(profd_info.value_size);
    if (get_global_data(bpf_obj_get(args->pin[1]), &profd_info, profd_data))
    {
        log_fata(args, "could not get global data from map '%s'\n", args->pin[1]);
    }
    fwrite(profd_data, profd_info.value_size, 1, outfp);

    /* Write the counters part */
    log_info(args, "%s\n", "about to write the counters in the profraw..");
    void *profc_data = malloc(profc_info.value_size);
    if (get_global_data(bpf_obj_get(args->pin[0]), &profc_info, profc_data))
    {
        log_fata(args, "could not get global data from map '%s'\n", args->pin[0]);
    }
    fwrite(profc_data, profc_info.value_size, 1, outfp);

    /* Write the names part */

    // int profd_fd = bpf_obj_get(args->pin[1]);
    // log_info(args, "fd: %d (%s)\n", profd_fd, strerror(errno));

    // struct bpf_map_info profd_map_info = {};
    // memset(&profd_map_info, 0, sizeof(profd_map_info));
    // unsigned int len = sizeof(profd_map_info);

    // int err;
    // err = bpf_obj_get_info_by_fd(profd_fd, &profd_map_info, &len);
    // if (!err)
    // {
    //     void *key = malloc(profd_map_info.key_size);
    //     void *val = malloc(profd_map_info.value_size);
    //     // TODO > if (!key || !val) { ... }
    //     void *pkey;

    //     // long long int data_sz = profd_map_info.value_size / 48;

    //     for (;;)
    //     {
    //         err = bpf_map_get_next_key(profd_fd, pkey, key);
    //         if (err)
    //         {
    //             if (errno == ENOENT)
    //             {
    //                 err = 0;
    //             }
    //             break;
    //         }
    //         bpf_map_lookup_elem(profd_fd, key, val);
    //         unsigned int i;
    //         for (i = 0; i < profd_map_info.value_size; i++)
    //         {
    //             printf("%02x", ((char *)val)[i]);
    //         }
    //         pkey = key;
    //     }
    // }

    // TODO >> SEE BPFTOOL MAP DUMP SOURCE

    // const long long int key = 0;
    // void* val = 0;
    // int ret = bpf_map_lookup_elem(profd_fd, &key, &val);

    // log_info(args, "fd: l->(%u):%u ret:(%d,%s)\n", key, val, ret, strerror(errno));

cleanup:
    fclose(outfp);

    return 0;
}