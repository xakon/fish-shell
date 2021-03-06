/** \file input.c

    Functions for reading a character of input from stdin.

*/

#include "config.h"

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <errno.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifdef HAVE_SYS_IOCTL_H
#include <sys/ioctl.h>
#endif

#include <unistd.h>
#include <wchar.h>

#if HAVE_NCURSES_H
#include <ncurses.h>
#else
#include <curses.h>
#endif

#if HAVE_TERM_H
#include <term.h>
#elif HAVE_NCURSES_TERM_H
#include <ncurses/term.h>
#endif

#include <signal.h>
#include <dirent.h>
#include <wctype.h>



#include "fallback.h"
#include "util.h"

#include "wutil.h"
#include "reader.h"
#include "proc.h"
#include "common.h"
#include "sanity.h"
#include "input_common.h"
#include "input.h"
#include "parser.h"
#include "env.h"
#include "expand.h"
#include "event.h"
#include "signal.h"

#include "output.h"
#include "intern.h"
#include <vector>
#include <algorithm>

#define DEFAULT_TERM L"ansi"
#define MAX_INPUT_FUNCTION_ARGS 20

/**
   Struct representing a keybinding. Returned by input_get_mappings.
 */

struct input_mapping_t
{
    wcstring seq; /**< Character sequence which generates this event */
    wcstring_list_t commands; /**< commands that should be evaluated by this mapping */

    /* We wish to preserve the user-specified order. This is just an incrementing value. */
    unsigned int specification_order;

    wcstring mode; /**< mode in which this command should be evaluated */
    wcstring sets_mode; /** new mode that should be switched to after command evaluation */

    input_mapping_t(const wcstring &s, const std::vector<wcstring> &c,
                    const wcstring &m = DEFAULT_BIND_MODE,
                    const wcstring &sm = DEFAULT_BIND_MODE) : seq(s), commands(c), mode(m), sets_mode(sm)
    {
        static unsigned int s_last_input_mapping_specification_order = 0;
        specification_order = ++s_last_input_mapping_specification_order;

    }
};

/**
   A struct representing the mapping from a terminfo key name to a terminfo character sequence
 */
struct terminfo_mapping_t
{
    const wchar_t *name; /**< Name of key */
    const char *seq; /**< Character sequence generated on keypress. Constant string. */
};


/**
   Names of all the input functions supported
*/
static const wchar_t * const name_arr[] =
{
    L"beginning-of-line",
    L"end-of-line",
    L"forward-char",
    L"backward-char",
    L"forward-word",
    L"backward-word",
    L"history-search-backward",
    L"history-search-forward",
    L"delete-char",
    L"backward-delete-char",
    L"kill-line",
    L"yank",
    L"yank-pop",
    L"complete",
    L"complete-and-search",
    L"beginning-of-history",
    L"end-of-history",
    L"backward-kill-line",
    L"kill-whole-line",
    L"kill-word",
    L"backward-kill-word",
    L"backward-kill-path-component",
    L"history-token-search-backward",
    L"history-token-search-forward",
    L"self-insert",
    L"transpose-chars",
    L"transpose-words",
    L"upcase-word",
    L"downcase-word",
    L"capitalize-word",
    L"vi-arg-digit",
    L"vi-delete-to",
    L"execute",
    L"beginning-of-buffer",
    L"end-of-buffer",
    L"repaint",
    L"force-repaint",
    L"up-line",
    L"down-line",
    L"suppress-autosuggestion",
    L"accept-autosuggestion",
    L"begin-selection",
    L"end-selection",
    L"kill-selection",
    L"forward-jump",
    L"backward-jump",
    L"and",
    L"cancel"
};

wcstring describe_char(wint_t c)
{
    wint_t initial_cmd_char = R_BEGINNING_OF_LINE;
    size_t name_count = sizeof name_arr / sizeof *name_arr;
    if (c >= initial_cmd_char && c < initial_cmd_char + name_count)
    {
        return format_string(L"%02x (%ls)", c, name_arr[c - initial_cmd_char]);
    }
    return format_string(L"%02x", c);
}

/**
   Description of each supported input function
*/
/*
static const wchar_t *desc_arr[] =
{
  L"Move to beginning of line",
  L"Move to end of line",
  L"Move forward one character",
  L"Move backward one character",
  L"Move forward one word",
  L"Move backward one word",
  L"Search backward through list of previous commands",
  L"Search forward through list of previous commands",
  L"Delete one character forward",
  L"Delete one character backward",
  L"Move contents from cursor to end of line to killring",
  L"Paste contents of killring",
  L"Rotate to previous killring entry",
  L"Guess the rest of the next input token",
  L"Move to first item of history",
  L"Move to last item of history",
  L"Clear current line",
  L"Move contents from beginning of line to cursor to killring",
  L"Move entire line to killring",
  L"Move next word to killring",
  L"Move previous word to killring",
  L"Write out key bindings",
  L"Clear entire screen",
  L"Quit the running program",
  L"Search backward through list of previous commands for matching token",
  L"Search forward through list of previous commands for matching token",
  L"Insert the pressed key",
  L"Do nothing",
  L"End of file",
  L"Repeat command"
}
  ;
*/

/**
   Internal code for each supported input function
*/
static const wchar_t code_arr[] =
{
    R_BEGINNING_OF_LINE,
    R_END_OF_LINE,
    R_FORWARD_CHAR,
    R_BACKWARD_CHAR,
    R_FORWARD_WORD,
    R_BACKWARD_WORD,
    R_HISTORY_SEARCH_BACKWARD,
    R_HISTORY_SEARCH_FORWARD,
    R_DELETE_CHAR,
    R_BACKWARD_DELETE_CHAR,
    R_KILL_LINE,
    R_YANK,
    R_YANK_POP,
    R_COMPLETE,
    R_COMPLETE_AND_SEARCH,
    R_BEGINNING_OF_HISTORY,
    R_END_OF_HISTORY,
    R_BACKWARD_KILL_LINE,
    R_KILL_WHOLE_LINE,
    R_KILL_WORD,
    R_BACKWARD_KILL_WORD,
    R_BACKWARD_KILL_PATH_COMPONENT,
    R_HISTORY_TOKEN_SEARCH_BACKWARD,
    R_HISTORY_TOKEN_SEARCH_FORWARD,
    R_SELF_INSERT,
    R_TRANSPOSE_CHARS,
    R_TRANSPOSE_WORDS,
    R_UPCASE_WORD,
    R_DOWNCASE_WORD,
    R_CAPITALIZE_WORD,
    R_VI_ARG_DIGIT,
    R_VI_DELETE_TO,
    R_EXECUTE,
    R_BEGINNING_OF_BUFFER,
    R_END_OF_BUFFER,
    R_REPAINT,
    R_FORCE_REPAINT,
    R_UP_LINE,
    R_DOWN_LINE,
    R_SUPPRESS_AUTOSUGGESTION,
    R_ACCEPT_AUTOSUGGESTION,
    R_BEGIN_SELECTION,
    R_END_SELECTION,
    R_KILL_SELECTION,
    R_FORWARD_JUMP,
    R_BACKWARD_JUMP,
    R_AND,
    R_CANCEL
};

/** Mappings for the current input mode */
static std::vector<input_mapping_t> mapping_list;

/* Terminfo map list */
static std::vector<terminfo_mapping_t> terminfo_mappings;

#define TERMINFO_ADD(key) { (L ## #key) + 4, key }


/**
   List of all terminfo mappings
 */
static std::vector<terminfo_mapping_t> mappings;


/**
   Set to one when the input subsytem has been initialized.
*/
static bool is_init = false;

/**
   Initialize terminfo.
 */
static void input_terminfo_init();

static wchar_t input_function_args[MAX_INPUT_FUNCTION_ARGS];
static bool input_function_status;
static int input_function_args_index = 0;

/**
    Return the current bind mode
*/
wcstring input_get_bind_mode()
{
    env_var_t mode = env_get_string(FISH_BIND_MODE_VAR);
    return mode.missing() ? DEFAULT_BIND_MODE : mode;
}

/**
    Set the current bind mode
*/
void input_set_bind_mode(const wcstring &bm)
{
    env_set(FISH_BIND_MODE_VAR, bm.c_str(), ENV_GLOBAL);
}


/**
    Returns the arity of a given input function
*/
int input_function_arity(int function)
{
    switch (function)
    {
        case R_FORWARD_JUMP:
        case R_BACKWARD_JUMP:
            return 1;
        default:
            return 0;
    }
}

/**
    Sets the return status of the most recently executed input function
*/
void input_function_set_status(bool status)
{
    input_function_status = status;
}

/**
    Returns the nth argument for a given input function
*/
wchar_t input_function_get_arg(int index)
{
    return input_function_args[index];
}

/* Helper function to compare the lengths of sequences */
static bool length_is_greater_than(const input_mapping_t &m1, const input_mapping_t &m2)
{
    return m1.seq.size() > m2.seq.size();
}

static bool specification_order_is_less_than(const input_mapping_t &m1, const input_mapping_t &m2)
{
    return m1.specification_order < m2.specification_order;
}

/* Inserts an input mapping at the correct position. We sort them in descending order by length, so that we test longer sequences first. */
static void input_mapping_insert_sorted(const input_mapping_t &new_mapping)
{
    std::vector<input_mapping_t>::iterator loc = std::lower_bound(mapping_list.begin(), mapping_list.end(), new_mapping, length_is_greater_than);
    mapping_list.insert(loc, new_mapping);
}

/* Adds an input mapping */
void input_mapping_add(const wchar_t *sequence, const wchar_t **commands, size_t commands_len,
                       const wchar_t *mode, const wchar_t *sets_mode)
{
    CHECK(sequence,);
    CHECK(commands,);
    CHECK(mode,);
    CHECK(sets_mode,);

    // debug( 0, L"Add mapping from %ls to %ls in mode %ls", escape(sequence, 1), escape(command, 1 ), mode);

    // remove existing mappings with this sequence
    const wcstring_list_t commands_vector(commands, commands + commands_len);

    for (size_t i=0; i<mapping_list.size(); i++)
    {
        input_mapping_t &m = mapping_list.at(i);
        if (m.seq == sequence && m.mode == mode)
        {
            m.commands = commands_vector;
            m.sets_mode = sets_mode;
            return;
        }
    }

    // add a new mapping, using the next order
    const input_mapping_t new_mapping = input_mapping_t(sequence, commands_vector, mode, sets_mode);
    input_mapping_insert_sorted(new_mapping);
}

void input_mapping_add(const wchar_t *sequence, const wchar_t *command,
                       const wchar_t *mode, const wchar_t *sets_mode)
{
    input_mapping_add(sequence, &command, 1, mode, sets_mode);
}

/**
   Handle interruptions to key reading by reaping finshed jobs and
   propagating the interrupt to the reader.
*/
static int interrupt_handler()
{
    /*
      Fire any pending events
    */
    event_fire(NULL);

    /*
      Reap stray processes, including printing exit status messages
    */
    if (job_reap(1))
        reader_repaint_needed();

    /*
      Tell the reader an event occured
    */
    if (reader_reading_interrupted())
    {
        /*
          Return 3, i.e. the character read by a Control-C.
        */
        return 3;
    }

    return R_NULL;
}

void update_fish_term256(void)
{
    /* Infer term256 support. If fish_term256 is set, we respect it; otherwise try to detect it from the TERM variable */
    env_var_t fish_term256 = env_get_string(L"fish_term256");
    bool support_term256;
    if (! fish_term256.missing_or_empty())
    {
        support_term256 = from_string<bool>(fish_term256);
    }
    else
    {
        env_var_t term = env_get_string(L"TERM");
        if (term.missing())
        {
            support_term256 = false;
        }
        else if (term.find(L"256color") != wcstring::npos)
        {
            /* Explicitly supported */
            support_term256 = true;
        }
        else if (term.find(L"xterm") != wcstring::npos)
        {
            // assume that all xterms are 256, except for OS X SnowLeopard
            env_var_t prog = env_get_string(L"TERM_PROGRAM");
            support_term256 = (prog != L"Apple_Terminal");
        }
        else
        {
            // Don't know, default to false
            support_term256 = false;
        }
    }
    output_set_supports_term256(support_term256);
}

int input_init()
{
    if (is_init)
        return 1;

    is_init = true;

    input_common_init(&interrupt_handler);

    const env_var_t term = env_get_string(L"TERM");
    int errret;
    if (setupterm(0, STDOUT_FILENO, &errret) == ERR)
    {
        debug(0, _(L"Could not set up terminal"));
        if (errret == 0)
        {
            debug(0, _(L"Check that your terminal type, '%ls', is supported on this system"),
                  term.c_str());
            debug(0, _(L"Attempting to use '%ls' instead"), DEFAULT_TERM);
            env_set(L"TERM", DEFAULT_TERM, ENV_GLOBAL | ENV_EXPORT);
            const std::string default_term = wcs2string(DEFAULT_TERM);
            if (setupterm(const_cast<char *>(default_term.c_str()), STDOUT_FILENO, &errret) == ERR)
            {
                debug(0, _(L"Could not set up terminal"));
                exit_without_destructors(1);
            }
        }
        else
        {
            exit_without_destructors(1);
        }
    }
    assert(! term.missing());
    output_set_term(term);

    input_terminfo_init();

    update_fish_term256();

    /* If we have no keybindings, add a few simple defaults */
    if (mapping_list.empty())
    {
        input_mapping_add(L"", L"self-insert");
        input_mapping_add(L"\n", L"execute");
        input_mapping_add(L"\t", L"complete");
        input_mapping_add(L"\x3", L"commandline \"\"");
        input_mapping_add(L"\x4", L"exit");
        input_mapping_add(L"\x5", L"bind");
    }

    return 1;
}

void input_destroy()
{
    if (!is_init)
        return;


    is_init = false;

    input_common_destroy();

    if (fish_del_curterm(cur_term) == ERR)
    {
        debug(0, _(L"Error while closing terminfo"));
    }
}

void input_function_push_arg(wchar_t arg)
{
    input_function_args[input_function_args_index++] = arg;
}

wchar_t input_function_pop_arg()
{
    return input_function_args[--input_function_args_index];
}

void input_function_push_args(int code)
{
    int arity = input_function_arity(code);
    for (int i = 0; i < arity; i++)
    {
        input_function_push_arg(input_common_readch(0));
    }
}

/**
   Perform the action of the specified binding
   allow_commands controls whether fish commands should be executed, or should
   be deferred until later.
*/
static void input_mapping_execute(const input_mapping_t &m, bool allow_commands)
{
    /* By default input functions always succeed */
    input_function_status = true;

    size_t idx = m.commands.size();
    while (idx--)
    {
        wcstring command = m.commands.at(idx);
        wchar_t code = input_function_get_code(command);
        if (code != (wchar_t)-1)
        {
            input_function_push_args(code);
        }
    }

    idx = m.commands.size();
    while (idx--)
    {
        wcstring command = m.commands.at(idx);
        wchar_t code = input_function_get_code(command);
        if (code != (wchar_t)-1)
        {
            input_unreadch(code);
        }
        else if (allow_commands)
        {
            /*
             This key sequence is bound to a command, which
             is sent to the parser for evaluation.
             */
            int last_status = proc_get_last_status();
            parser_t::principal_parser().eval(command.c_str(), io_chain_t(), TOP);

            proc_set_last_status(last_status);

            input_unreadch(R_NULL);
        }
        else
        {
            /* We don't want to run commands yet. Put the characters back and return R_NULL */
            for (wcstring::const_reverse_iterator it = m.seq.rbegin(), end = m.seq.rend(); it != end; ++it)
            {
                input_unreadch(*it);
            }
            input_unreadch(R_NULL);
            return; /* skip the input_set_bind_mode */
        }
    }

    input_set_bind_mode(m.sets_mode.c_str());
}



/**
   Try reading the specified function mapping
*/
static bool input_mapping_is_match(const input_mapping_t &m)
{
    wint_t c = 0;
    int j;

    //debug(0, L"trying mapping %ls\n", escape(m.seq.c_str(), 1));
    const wchar_t *str = m.seq.c_str();
    for (j=0; str[j] != L'\0'; j++)
    {
        bool timed = (j > 0 && iswcntrl(str[0]));

        c = input_common_readch(timed);
        if (str[j] != c)
        {
            break;
        }
    }

    if (str[j] == L'\0')
    {
        //debug(0, L"matched mapping %ls (%ls)\n", escape(m.seq.c_str(), 1), m.command.c_str());
        /* We matched the entire sequence */
        return true;
    }
    else
    {
        int k;
        /*
          Return the read characters
        */
        input_unreadch(c);
        for (k=j-1; k>=0; k--)
        {
            input_unreadch(m.seq[k]);
        }
    }

    return false;

}

void input_unreadch(wint_t ch)
{
    input_common_unreadch(ch);
}

static void input_mapping_execute_matching_or_generic(bool allow_commands)
{
    const input_mapping_t *generic = NULL;

    const wcstring bind_mode = input_get_bind_mode();

    for (int i = 0; i < mapping_list.size(); i++)
    {
        const input_mapping_t &m = mapping_list.at(i);

        //debug(0, L"trying mapping (%ls,%ls,%ls)\n", escape(m.seq.c_str(), 1),
        //           m.mode.c_str(), m.sets_mode.c_str());

        if (m.mode != bind_mode)
        {
            //debug(0, L"skipping mapping because mode %ls != %ls\n", m.mode.c_str(), input_get_bind_mode());
            continue;
        }

        if (m.seq.length() == 0)
        {
            generic = &m;
        }
        else if (input_mapping_is_match(m))
        {
            input_mapping_execute(m, allow_commands);
            return;
        }
    }

    if (generic)
    {
        input_mapping_execute(*generic, allow_commands);
    }
    else
    {
        //debug(0, L"no generic found, ignoring...");
        wchar_t c = input_common_readch(0);
        if (c == R_EOF)
            input_common_unreadch(c);
    }
}

wint_t input_readch(bool allow_commands)
{
    CHECK_BLOCK(R_NULL);

    /*
       Clear the interrupted flag
    */
    reader_reset_interrupted();

    /*
       Search for sequence in mapping tables
    */

    while (1)
    {
        wchar_t c = input_common_readch(0);

        if (c >= R_MIN && c <= R_MAX)
        {
            switch (c)
            {
                case R_EOF: /* If it's closed, then just return */
                {
                    return R_EOF;
                }
                case R_SELF_INSERT:
                {
                    return input_common_readch(0);
                }
                case R_AND:
                {
                    if (input_function_status)
                    {
                        return input_readch();
                    }
                    else
                    {
                        while ((c = input_common_readch(0)) && c >= R_MIN && c <= R_MAX);
                        input_unreadch(c);
                        return input_readch();
                    }
                }
                default:
                {
                    return c;
                }
            }
        }
        else
        {
            input_unreadch(c);
            input_mapping_execute_matching_or_generic(allow_commands);
            // regarding allow_commands, we're in a loop, but if a fish command
            // is executed, R_NULL is unread, so the next pass through the loop
            // we'll break out and return it.
        }
    }
}

wcstring_list_t input_mapping_get_names()
{
    // Sort the mappings by the user specification order, so we can return them in the same order that the user specified them in
    std::vector<input_mapping_t> local_list = mapping_list;
    std::sort(local_list.begin(), local_list.end(), specification_order_is_less_than);
    wcstring_list_t result;
    result.reserve(local_list.size());

    for (size_t i=0; i<local_list.size(); i++)
    {
        const input_mapping_t &m = local_list.at(i);
        result.push_back(m.seq);
    }
    return result;
}


bool input_mapping_erase(const wchar_t *sequence, const wchar_t *mode)
{
    ASSERT_IS_MAIN_THREAD();
    bool result = false;
    size_t i, sz = mapping_list.size();

    for (i=0; i<sz; i++)
    {
        const input_mapping_t &m = mapping_list.at(i);
        if (sequence == m.seq && (mode == NULL || mode == m.mode))
        {
            if (i != (sz-1))
            {
                mapping_list[i] = mapping_list[sz-1];
            }
            mapping_list.pop_back();
            result = true;
            break;

        }
    }
    return result;
}

bool input_mapping_get(const wcstring &sequence, wcstring_list_t *out_cmds, wcstring *out_mode, wcstring *out_sets_mode)
{
    bool result = false;
    size_t sz = mapping_list.size();
    for (size_t i=0; i<sz; i++)
    {
        const input_mapping_t &m = mapping_list.at(i);
        if (sequence == m.seq)
        {
            *out_cmds = m.commands;
            *out_mode = m.mode;
            *out_sets_mode = m.sets_mode;
            result = true;
            break;
        }
    }
    return result;
}

/**
   Add all terminfo mappings
 */
static void input_terminfo_init()
{
    const terminfo_mapping_t tinfos[] =
    {
        TERMINFO_ADD(key_a1),
        TERMINFO_ADD(key_a3),
        TERMINFO_ADD(key_b2),
        TERMINFO_ADD(key_backspace),
        TERMINFO_ADD(key_beg),
        TERMINFO_ADD(key_btab),
        TERMINFO_ADD(key_c1),
        TERMINFO_ADD(key_c3),
        TERMINFO_ADD(key_cancel),
        TERMINFO_ADD(key_catab),
        TERMINFO_ADD(key_clear),
        TERMINFO_ADD(key_close),
        TERMINFO_ADD(key_command),
        TERMINFO_ADD(key_copy),
        TERMINFO_ADD(key_create),
        TERMINFO_ADD(key_ctab),
        TERMINFO_ADD(key_dc),
        TERMINFO_ADD(key_dl),
        TERMINFO_ADD(key_down),
        TERMINFO_ADD(key_eic),
        TERMINFO_ADD(key_end),
        TERMINFO_ADD(key_enter),
        TERMINFO_ADD(key_eol),
        TERMINFO_ADD(key_eos),
        TERMINFO_ADD(key_exit),
        TERMINFO_ADD(key_f0),
        TERMINFO_ADD(key_f1),
        TERMINFO_ADD(key_f2),
        TERMINFO_ADD(key_f3),
        TERMINFO_ADD(key_f4),
        TERMINFO_ADD(key_f5),
        TERMINFO_ADD(key_f6),
        TERMINFO_ADD(key_f7),
        TERMINFO_ADD(key_f8),
        TERMINFO_ADD(key_f9),
        TERMINFO_ADD(key_f10),
        TERMINFO_ADD(key_f11),
        TERMINFO_ADD(key_f12),
        TERMINFO_ADD(key_f13),
        TERMINFO_ADD(key_f14),
        TERMINFO_ADD(key_f15),
        TERMINFO_ADD(key_f16),
        TERMINFO_ADD(key_f17),
        TERMINFO_ADD(key_f18),
        TERMINFO_ADD(key_f19),
        TERMINFO_ADD(key_f20),
        /*
        I know of no keyboard with more than 20 function keys, so
        adding the rest here makes very little sense, since it will
        take up a lot of room in any listings (like tab completions),
        but with no benefit.
        */
        /*
        TERMINFO_ADD(key_f21),
        TERMINFO_ADD(key_f22),
        TERMINFO_ADD(key_f23),
        TERMINFO_ADD(key_f24),
        TERMINFO_ADD(key_f25),
        TERMINFO_ADD(key_f26),
        TERMINFO_ADD(key_f27),
        TERMINFO_ADD(key_f28),
        TERMINFO_ADD(key_f29),
        TERMINFO_ADD(key_f30),
        TERMINFO_ADD(key_f31),
        TERMINFO_ADD(key_f32),
        TERMINFO_ADD(key_f33),
        TERMINFO_ADD(key_f34),
        TERMINFO_ADD(key_f35),
        TERMINFO_ADD(key_f36),
        TERMINFO_ADD(key_f37),
        TERMINFO_ADD(key_f38),
        TERMINFO_ADD(key_f39),
        TERMINFO_ADD(key_f40),
        TERMINFO_ADD(key_f41),
        TERMINFO_ADD(key_f42),
        TERMINFO_ADD(key_f43),
        TERMINFO_ADD(key_f44),
        TERMINFO_ADD(key_f45),
        TERMINFO_ADD(key_f46),
        TERMINFO_ADD(key_f47),
        TERMINFO_ADD(key_f48),
        TERMINFO_ADD(key_f49),
        TERMINFO_ADD(key_f50),
        TERMINFO_ADD(key_f51),
        TERMINFO_ADD(key_f52),
        TERMINFO_ADD(key_f53),
        TERMINFO_ADD(key_f54),
        TERMINFO_ADD(key_f55),
        TERMINFO_ADD(key_f56),
        TERMINFO_ADD(key_f57),
        TERMINFO_ADD(key_f58),
        TERMINFO_ADD(key_f59),
        TERMINFO_ADD(key_f60),
        TERMINFO_ADD(key_f61),
        TERMINFO_ADD(key_f62),
        TERMINFO_ADD(key_f63),*/
        TERMINFO_ADD(key_find),
        TERMINFO_ADD(key_help),
        TERMINFO_ADD(key_home),
        TERMINFO_ADD(key_ic),
        TERMINFO_ADD(key_il),
        TERMINFO_ADD(key_left),
        TERMINFO_ADD(key_ll),
        TERMINFO_ADD(key_mark),
        TERMINFO_ADD(key_message),
        TERMINFO_ADD(key_move),
        TERMINFO_ADD(key_next),
        TERMINFO_ADD(key_npage),
        TERMINFO_ADD(key_open),
        TERMINFO_ADD(key_options),
        TERMINFO_ADD(key_ppage),
        TERMINFO_ADD(key_previous),
        TERMINFO_ADD(key_print),
        TERMINFO_ADD(key_redo),
        TERMINFO_ADD(key_reference),
        TERMINFO_ADD(key_refresh),
        TERMINFO_ADD(key_replace),
        TERMINFO_ADD(key_restart),
        TERMINFO_ADD(key_resume),
        TERMINFO_ADD(key_right),
        TERMINFO_ADD(key_save),
        TERMINFO_ADD(key_sbeg),
        TERMINFO_ADD(key_scancel),
        TERMINFO_ADD(key_scommand),
        TERMINFO_ADD(key_scopy),
        TERMINFO_ADD(key_screate),
        TERMINFO_ADD(key_sdc),
        TERMINFO_ADD(key_sdl),
        TERMINFO_ADD(key_select),
        TERMINFO_ADD(key_send),
        TERMINFO_ADD(key_seol),
        TERMINFO_ADD(key_sexit),
        TERMINFO_ADD(key_sf),
        TERMINFO_ADD(key_sfind),
        TERMINFO_ADD(key_shelp),
        TERMINFO_ADD(key_shome),
        TERMINFO_ADD(key_sic),
        TERMINFO_ADD(key_sleft),
        TERMINFO_ADD(key_smessage),
        TERMINFO_ADD(key_smove),
        TERMINFO_ADD(key_snext),
        TERMINFO_ADD(key_soptions),
        TERMINFO_ADD(key_sprevious),
        TERMINFO_ADD(key_sprint),
        TERMINFO_ADD(key_sr),
        TERMINFO_ADD(key_sredo),
        TERMINFO_ADD(key_sreplace),
        TERMINFO_ADD(key_sright),
        TERMINFO_ADD(key_srsume),
        TERMINFO_ADD(key_ssave),
        TERMINFO_ADD(key_ssuspend),
        TERMINFO_ADD(key_stab),
        TERMINFO_ADD(key_sundo),
        TERMINFO_ADD(key_suspend),
        TERMINFO_ADD(key_undo),
        TERMINFO_ADD(key_up)
    };
    const size_t count = sizeof tinfos / sizeof *tinfos;
    terminfo_mappings.reserve(terminfo_mappings.size() + count);
    terminfo_mappings.insert(terminfo_mappings.end(), tinfos, tinfos + count);
}

bool input_terminfo_get_sequence(const wchar_t *name, wcstring *out_seq)
{
    ASSERT_IS_MAIN_THREAD();

    const char *res = 0;
    int err = ENOENT;

    CHECK(name, 0);
    input_init();

    for (size_t i=0; i<terminfo_mappings.size(); i++)
    {
        const terminfo_mapping_t &m = terminfo_mappings.at(i);
        if (!wcscmp(name, m.name))
        {
            res = m.seq;
            err = EILSEQ;
            break;
        }
    }

    if (!res)
    {
        errno = err;
        return false;
    }

    *out_seq = format_string(L"%s", res);
    return true;

}

bool input_terminfo_get_name(const wcstring &seq, wcstring *out_name)
{
    input_init();

    for (size_t i=0; i<terminfo_mappings.size(); i++)
    {
        terminfo_mapping_t &m = terminfo_mappings.at(i);

        if (!m.seq)
        {
            continue;
        }

        const wcstring map_buf = format_string(L"%s",  m.seq);
        if (map_buf == seq)
        {
            out_name->assign(m.name);
            return true;
        }
    }

    return false;
}

wcstring_list_t input_terminfo_get_names(bool skip_null)
{
    wcstring_list_t result;
    result.reserve(terminfo_mappings.size());

    input_init();

    for (size_t i=0; i<terminfo_mappings.size(); i++)
    {
        terminfo_mapping_t &m = terminfo_mappings.at(i);

        if (skip_null && !m.seq)
        {
            continue;
        }
        result.push_back(wcstring(m.name));
    }
    return result;
}

wcstring_list_t input_function_get_names(void)
{
    size_t count = sizeof name_arr / sizeof *name_arr;
    return wcstring_list_t(name_arr, name_arr + count);
}

wchar_t input_function_get_code(const wcstring &name)
{

    size_t i;
    for (i = 0; i<(sizeof(code_arr)/sizeof(wchar_t)) ; i++)
    {
        if (name == name_arr[i])
        {
            return code_arr[i];
        }
    }
    return -1;
}
