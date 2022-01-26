// Copyright (c) 2015 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "lua_state.h"
#include "prompt.h"
#include "../../app/src/version.h" // Ugh.

#include <core/base.h>
#include <core/log.h>
#include <core/os.h>
#include <core/path.h>
#include <core/str.h>
#include <core/str_compare.h>
#include <core/str_iter.h>
#include <core/str_transform.h>
#include <core/str_tokeniser.h>
#include <core/str_unordered_set.h>
#include <core/settings.h>
#include <core/linear_allocator.h>
#include <core/debugheap.h>
#include <lib/intercept.h>
#include <lib/popup.h>
#include <terminal/terminal_helpers.h>
#include <terminal/printer.h>
#include <terminal/screen_buffer.h>
#include <readline/readline.h>

extern "C" {
#include <lua.h>
#include <readline/history.h>
}

#include <list>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>



//------------------------------------------------------------------------------
extern int force_reload_scripts();
extern void host_reclassify();
extern void set_suggestion(const char* line, unsigned int endword_offset, const char* suggestion, unsigned int offset);
extern setting_bool g_gui_popups;
extern setting_enum g_dupe_mode;
extern setting_color g_color_unrecognized;
extern setting_color g_color_executable;



//------------------------------------------------------------------------------
static bool search_for_extension(str_base& full, const char* word)
{
    path::append(full, "");
    const unsigned int trunc = full.length();

    if (strchr(word, '.'))
    {
        path::append(full, word);
        if (os::get_path_type(full.c_str()) == os::path_type_file)
            return true;
    }
    else
    {
        str<> pathext;
        if (!os::get_env("pathext", pathext))
            return false;

        str_tokeniser tokens(pathext.c_str(), ";");
        const char *start;
        int length;

        while (str_token token = tokens.next(start, length))
        {
            full.truncate(trunc);
            path::append(full, word);
            full.concat(start, length);
            if (os::get_path_type(full.c_str()) == os::path_type_file)
                return true;
        }
    }

    return false;
}

//------------------------------------------------------------------------------
static bool search_for_executable(const char* _word)
{
    // Bail out early if it's obviously not going to succeed.
    if (strlen(_word) >= MAX_PATH)
        return false;

// TODO: dynamically load NeedCurrentDirectoryForExePathW.
    wstr<32> word(_word);
    const bool need_cwd = !!NeedCurrentDirectoryForExePathW(word.c_str());
    const bool need_path = !rl_last_path_separator(_word);

    // Make list of paths to search.
    str<> tmp;
    str<> paths;
    if (need_cwd)
    {
        os::get_current_dir(paths);
    }
    if (need_path && os::get_env("PATH", tmp))
    {
        if (paths.length() > 0)
            paths.concat(";", 1);
        paths.concat(tmp.c_str(), tmp.length());
    }

    str<280> token;
    str_tokeniser tokens(paths.c_str(), ";");
    while (tokens.next(token))
    {
        token.trim();
        if (token.empty())
            continue;

        // Get full path name.
        str<> full;
        if (!os::get_full_path_name(token.c_str(), full, token.length()))
            continue;

        // Skip drives that are unknown, invalid, or remote.
        {
            char drive[4];
            drive[0] = full.c_str()[0];
            drive[1] = ':';
            drive[2] = '\\';
            drive[3] = '\0';
            if (os::get_drive_type(drive) < os::drive_type_removable)
                continue;
        }

        // Try PATHEXT extensions.
        if (search_for_extension(full, _word))
            return true;
    }

    return false;
}

//------------------------------------------------------------------------------
class recognizer
{
    friend HANDLE get_recognizer_event();

    struct entry
    {
                            entry() {}
                            entry(const char* key, const char* word);
        bool                empty() const { return m_key.empty(); }
        void                clear();
        str_moveable        m_key;
        str_moveable        m_word;
    };

public:
                            recognizer();
                            ~recognizer() { shutdown(); }
    void                    clear();
    bool                    find(const char* key, char* cached=nullptr) const;
    bool                    enqueue(const char* key, const char* word, char* cached=nullptr);
    bool                    need_refresh();
    void                    end_line();

private:
    bool                    usable() const;
    bool                    store(const char* word, char cached, bool pending=false);
    bool                    dequeue(entry& entry);
    bool                    set_result_available(bool available);
    void                    notify_ready(bool available);
    void                    shutdown();
    static void             proc(recognizer* r);

private:
    linear_allocator        m_heap;
    str_unordered_map<char> m_cache;
    str_unordered_map<char> m_pending;
    entry                   m_queue;
    mutable std::recursive_mutex m_mutex;
    std::unique_ptr<std::thread> m_thread;
    HANDLE                  m_event = nullptr;
    bool                    m_processing = false;
    bool                    m_result_available = false;
    bool                    m_zombie = false;

    static HANDLE           s_ready_event;
};

//------------------------------------------------------------------------------
HANDLE recognizer::s_ready_event = nullptr;
static recognizer s_recognizer;

//------------------------------------------------------------------------------
HANDLE get_recognizer_event()
{
    str<32> tmp;
    g_color_unrecognized.get_descriptive(tmp);
    if (tmp.empty())
    {
        str<32> tmp2;
        g_color_executable.get_descriptive(tmp2);
        if (tmp2.empty())
            return nullptr;
    }

    // Locking is not needed because concurrency is not possible until after
    // this event has been created, which can only happen on the main thread.

    if (s_recognizer.m_zombie)
        return nullptr;
    return s_recognizer.s_ready_event;
}

//------------------------------------------------------------------------------
bool check_recognizer_refresh()
{
    return s_recognizer.need_refresh();
}

//------------------------------------------------------------------------------
extern "C" void end_recognizer()
{
    s_recognizer.end_line();
    s_recognizer.clear();
}

//------------------------------------------------------------------------------
recognizer::entry::entry(const char* key, const char* word)
: m_key(key)
, m_word(word)
{
}

//------------------------------------------------------------------------------
void recognizer::entry::clear()
{
    m_key.clear();
    m_word.clear();
}

//------------------------------------------------------------------------------
recognizer::recognizer()
: m_heap(1024)
{
#ifdef DEBUG
    // Singleton; assert if there's ever more than one.
    static bool s_created = false;
    assert(!s_created);
    s_created = true;
#endif

    s_recognizer.s_ready_event = CreateEvent(nullptr, true, false, nullptr);
}

//------------------------------------------------------------------------------
void recognizer::clear()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    m_cache.clear();
    m_pending.clear();
    m_heap.reset();
}

//------------------------------------------------------------------------------
bool recognizer::find(const char* key, char* cached) const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (usable())
    {
        auto const iter = m_cache.find(key);
        if (iter != m_cache.end())
        {
            if (cached)
                *cached = iter->second;
            return true;
        }
    }

    if (usable())
    {
        auto const iter = m_pending.find(key);
        if (iter != m_pending.end())
        {
            if (cached)
                *cached = iter->second;
            return true;
        }
    }

    return false;
}

//------------------------------------------------------------------------------
bool recognizer::enqueue(const char* key, const char* word, char* cached)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!usable())
        return false;

    assert(s_ready_event);

    if (!m_event)
    {
        m_event = CreateEvent(nullptr, false, false, nullptr);
        if (!m_event)
            return false;
    }

    if (!m_thread)
    {
        dbg_ignore_scope(snapshot, "Recognizer thread");
        m_thread = std::make_unique<std::thread>(&proc, this);
    }

    m_queue.m_key = key;
    m_queue.m_word = word;

    // Assume unrecognized at first.
    store(key, -1, true/*pending*/);
    if (cached)
        *cached = -1;

    SetEvent(m_event);  // Signal thread there is work to do.
    Sleep(0);           // Give up timeslice in case thread gets result quickly.
    return true;
}

//------------------------------------------------------------------------------
bool recognizer::need_refresh()
{
    return set_result_available(false);
}

//------------------------------------------------------------------------------
void recognizer::end_line()
{
    HANDLE ready_event;
    bool processing;

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        ready_event = s_ready_event;
        processing = m_processing && !m_zombie;
        // s_ready_event is never closed, so there is no concurrency concern
        // about it going from non-null to null.
        if (!ready_event)
            return;
    }

    // If the recognizer is still processing something then wait briefly until
    // processing is finished, in case it finishes quickly enough to be able to
    // refresh the input line colors.
    if (processing)
    {
        const DWORD tick_begin = GetTickCount();
        while (true)
        {
            const volatile DWORD tick_now = GetTickCount();
            const int timeout = int(tick_begin) + 2500 - int(tick_now);
            if (timeout < 0)
                break;

            if (WaitForSingleObject(ready_event, DWORD(timeout)) != WAIT_OBJECT_0)
                break;

            host_reclassify();

            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            if (!m_processing || !usable())
                break;
        }
    }

    host_reclassify();
}

//------------------------------------------------------------------------------
bool recognizer::usable() const
{
    return !m_zombie && s_ready_event;
}

//------------------------------------------------------------------------------
bool recognizer::store(const char* word, char cached, bool pending)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!usable())
        return false;

    auto& map = pending ? m_pending : m_cache;

    auto const iter = map.find(word);
    if (iter != map.end())
    {
        map.insert_or_assign(iter->first, cached);
        set_result_available(true);
        return true;
    }

    dbg_ignore_scope(snapshot, "Recognizer");
    const char* key = m_heap.store(word);
    if (!key)
        return false;

    map.emplace(key, cached);
    set_result_available(true);
    return true;
}

//------------------------------------------------------------------------------
bool recognizer::dequeue(entry& entry)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (!usable() || m_queue.empty())
        return false;

    entry = std::move(m_queue);
    assert(m_queue.empty());
    return true;
}

//------------------------------------------------------------------------------
bool recognizer::set_result_available(const bool available)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (available == m_result_available)
        return available;

    m_result_available = available;

    if (s_ready_event)
    {
        if (available)
            SetEvent(s_ready_event);
        else
            ResetEvent(s_ready_event);
    }

    return !available;
}

//------------------------------------------------------------------------------
void recognizer::notify_ready(bool available)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);

    if (available)
        set_result_available(available);

    if (s_ready_event)
        SetEvent(s_ready_event);
}

//------------------------------------------------------------------------------
void recognizer::shutdown()
{
    std::unique_ptr<std::thread> thread;

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);

        clear();
        m_zombie = true;

        if (m_event)
            SetEvent(m_event);

        thread = std::move(m_thread);
    }

    if (thread)
        thread->join();

    if (m_event)
        CloseHandle(m_event);
}

//------------------------------------------------------------------------------
void recognizer::proc(recognizer* r)
{
    while (true)
    {
        if (WaitForSingleObject(r->m_event, INFINITE) != WAIT_OBJECT_0)
        {
            // Uh oh.
            Sleep(5000);
        }

        entry entry;
        while (true)
        {
            {
                std::lock_guard<std::recursive_mutex> lock(r->m_mutex);
                if (r->m_zombie || !r->dequeue(entry))
                {
                    r->m_processing = false;
                    r->m_pending.clear();
                    if (!r->m_zombie)
                        r->notify_ready(false);
                    break;
                }
                r->m_processing = true;
            }

            // Search for executable file.
            if (search_for_executable(entry.m_word.c_str()))
            {
executable:
                r->store(entry.m_key.c_str(), 1);
                r->notify_ready(true);
            }
            else if (const char* ext = path::get_extension(entry.m_word.c_str()))
            {
                // Look up file type association.
                HKEY hkey;
                wstr<64> commandkey(ext);
                commandkey << L"\\shell\\open\\command";
                if (RegOpenKeyExW(HKEY_CLASSES_ROOT, commandkey.c_str(), 0, MAXIMUM_ALLOWED, &hkey) == ERROR_SUCCESS)
                {
                    bool has_command = false;

                    DWORD type;
                    if (RegQueryValueExW(hkey, nullptr, 0, &type, nullptr, nullptr) == ERROR_MORE_DATA)
                        has_command = (type == REG_SZ || type == REG_EXPAND_SZ);
                    RegCloseKey(hkey);

                    if (has_command)
                        goto executable;
                }
            }
            else
            {
                // Not executable.
                r->store(entry.m_key.c_str(), -1);
                r->notify_ready(true);
            }
        }
    }
}



//------------------------------------------------------------------------------
/// -name:  clink.print
/// -ver:   1.2.11
/// -arg:   ...
/// This works like <code>print()</code>, but this supports ANSI escape codes.
///
/// If the special value <code>NONL</code> is included anywhere in the argument
/// list then the usual trailing newline is omitted.  This can sometimes be
/// useful particularly when printing certain ANSI escape codes.
///
/// <strong>Note:</strong>  In Clink versions before v1.2.11 the
/// <code>clink.print()</code> API exists (undocumented) but accepts exactly one
/// string argument and is therefore not fully compatible with normal
/// <code>print()</code> syntax.  If you use fewer or more than 1 argument or if
/// the argument is not a string, then first checking the Clink version (e.g.
/// <a href="#clink.version_encoded">clink.version_encoded</a>) can avoid
/// runtime errors.
/// -show:  clink.print("\x1b[32mgreen\x1b[m \x1b[35mmagenta\x1b[m")
/// -show:  -- Outputs <code>green</code> in green, a space, and <code>magenta</code> in magenta.
/// -show:
/// -show:  local a = "hello"
/// -show:  local world = 73
/// -show:  clink.print("a", a, "world", world)
/// -show:  -- Outputs <code>a       hello   world   73</code>.
/// -show:
/// -show:  clink.print("hello", NONL)
/// -show:  clink.print("world")
/// -show:  -- Outputs <code>helloworld</code>.
static int clink_print(lua_State* state)
{
    str<> out;
    bool nl = true;
    bool err = false;

    int n = lua_gettop(state);              // Number of arguments.
    lua_getglobal(state, "NONL");           // Special value `NONL`.
    lua_getglobal(state, "tostring");       // Function to convert to string (reused each loop iteration).

    for (int i = 1; i <= n; i++)
    {
        // Check for magic `NONL` value.
        if (lua_compare(state, -2, i, LUA_OPEQ))
        {
            nl = false;
            continue;
        }

        // Call function to convert arg to a string.
        lua_pushvalue(state, -1);           // Function to be called (tostring).
        lua_pushvalue(state, i);            // Value to print.
        if (lua_state::pcall(state, 1, 1) != 0)
        {
            if (const char* error = lua_tostring(state, -1))
            {
                puts("");
                puts(error);
            }
            return 0;
        }

        // Get result from the tostring call.
        size_t l;
        const char* s = lua_tolstring(state, -1, &l);
        if (s == NULL)
        {
            err = true;
            break;                          // Allow accumulated output to be printed before erroring out.
        }
        lua_pop(state, 1);                  // Pop result.

        // Add tab character to the output.
        if (i > 1)
            out << "\t";

        // Add string result to the output.
        out.concat(s, int(l));
    }

    if (g_printer)
    {
        if (nl)
            out.concat("\n");
        g_printer->print(out.c_str(), out.length());
    }
    else
    {
        printf("%s%s", out.c_str(), nl ? "\n" : "");
    }

    if (err)
        return luaL_error(state, LUA_QL("tostring") " must return a string to " LUA_QL("print"));

    return 0;
}

//------------------------------------------------------------------------------
/// -name:  clink.version_encoded
/// -ver:   1.1.10
/// -var:   integer
/// The Clink version number encoded as a single integer following the format
/// <span class="arg">Mmmmpppp</span> where <span class="arg">M</span> is the
/// major part, <span class="arg">m</span> is the minor part, and
/// <span class="arg">p</span> is the patch part of the version number.
///
/// For example, Clink v95.6.723 would be <code>950060723</code>.
///
/// This format makes it easy to test for feature availability by encoding
/// version numbers from the release notes.

//------------------------------------------------------------------------------
/// -name:  clink.version_major
/// -ver:   1.1.10
/// -var:   integer
/// The major part of the Clink version number.
/// For v<strong>1</strong>.2.3.a0f14d the major version is 1.

//------------------------------------------------------------------------------
/// -name:  clink.version_minor
/// -ver:   1.1.10
/// -var:   integer
/// The minor part of the Clink version number.
/// For v1.<strong>2</strong>.3.a0f14d the minor version is 2.

//------------------------------------------------------------------------------
/// -name:  clink.version_patch
/// -ver:   1.1.10
/// -var:   integer
/// The patch part of the Clink version number.
/// For v1.2.<strong>3</strong>.a0f14d the patch version is 3.

//------------------------------------------------------------------------------
/// -name:  clink.version_commit
/// -ver:   1.1.10
/// -var:   string
/// The commit part of the Clink version number.
/// For v1.2.3.<strong>a0f14d</strong> the commit part is a0f14d.



// BEGIN -- Clink 0.4.8 API compatibility --------------------------------------

extern "C" {
#include "lua.h"
#include <compat/config.h>
#include <readline/rlprivate.h>
}

extern int              get_clink_setting(lua_State* state);
extern int              glob_impl(lua_State* state, bool dirs_only, bool back_compat);
extern int              lua_execute(lua_State* state);

//------------------------------------------------------------------------------
int old_glob_dirs(lua_State* state)
{
    return glob_impl(state, true, true/*back_compat*/);
}

//------------------------------------------------------------------------------
int old_glob_files(lua_State* state)
{
    return glob_impl(state, false, true/*back_compat*/);
}

//------------------------------------------------------------------------------
static int get_setting_str(lua_State* state)
{
    return get_clink_setting(state);
}

//------------------------------------------------------------------------------
static int get_setting_int(lua_State* state)
{
    return get_clink_setting(state);
}

//------------------------------------------------------------------------------
static int get_rl_variable(lua_State* state)
{
    // Check we've got at least one string argument.
    if (lua_gettop(state) == 0 || !lua_isstring(state, 1))
        return 0;

    const char* string = lua_tostring(state, 1);
    const char* rl_cvar = rl_variable_value(string);
    if (rl_cvar == nullptr)
        return 0;

    lua_pushstring(state, rl_cvar);
    return 1;
}

//------------------------------------------------------------------------------
static int is_rl_variable_true(lua_State* state)
{
    int i;
    const char* cvar_value;

    i = get_rl_variable(state);
    if (i == 0)
    {
        return 0;
    }

    cvar_value = lua_tostring(state, -1);
    i = (_stricmp(cvar_value, "on") == 0) || (_stricmp(cvar_value, "1") == 0);
    lua_pop(state, 1);
    lua_pushboolean(state, i);

    return 1;
}

//------------------------------------------------------------------------------
static int get_host_process(lua_State* state)
{
    lua_pushstring(state, rl_readline_name);
    return 1;
}

//------------------------------------------------------------------------------
/// -name:  clink.split
/// -deprecated: string.explode
/// -arg:   str:string
/// -arg:   sep:string
/// -ret:   table



// END -- Clink 0.4.8 API compatibility ----------------------------------------



//------------------------------------------------------------------------------
/// -name:  clink.match_display_filter
/// -deprecated: builder:addmatch
/// -var:   function
/// This is no longer used.
/// -show:  clink.match_display_filter = function(matches)
/// -show:  &nbsp; -- Transform matches.
/// -show:  &nbsp; return matches
/// -show:  end

//------------------------------------------------------------------------------
static int map_string(lua_State* state, transform_mode mode)
{
    const char* string;
    int length;

    // Check we've got at least one argument...
    if (lua_gettop(state) == 0)
        return 0;

    // ...and that the argument is a string.
    if (!lua_isstring(state, 1))
        return 0;

    string = lua_tostring(state, 1);
    length = (int)strlen(string);

    wstr<> out;
    if (length)
    {
        wstr<> in(string);
        str_transform(in.c_str(), in.length(), out, mode);
    }

    if (_rl_completion_case_map)
    {
        for (unsigned int i = 0; i < out.length(); ++i)
        {
            if (out[i] == '-' && (mode != transform_mode::upper))
                out.data()[i] = '_';
            else if (out[i] == '_' && (mode == transform_mode::upper))
                out.data()[i] = '-';
        }
    }

    str<> text(out.c_str());

    lua_pushlstring(state, text.c_str(), text.length());

    return 1;
}

//------------------------------------------------------------------------------
/// -name:  clink.lower
/// -ver:   0.4.9
/// -arg:   text:string
/// -ret:   string
/// This API correctly converts UTF8 strings to lowercase, with international
/// linguistic awareness.
/// -show:  clink.lower("Hello World") -- returns "hello world"
static int to_lowercase(lua_State* state)
{
    return map_string(state, transform_mode::lower);
}

//------------------------------------------------------------------------------
/// -name:  clink.upper
/// -ver:   1.1.5
/// -arg:   text:string
/// -ret:   string
/// This API correctly converts UTF8 strings to uppercase, with international
/// linguistic awareness.
/// -show:  clink.upper("Hello World") -- returns "HELLO WORLD"
static int to_uppercase(lua_State* state)
{
    return map_string(state, transform_mode::upper);
}

//------------------------------------------------------------------------------
/// -name:  clink.popuplist
/// -ver:   1.2.17
/// -arg:   title:string
/// -arg:   items:table
/// -arg:   [index:integer]
/// -ret:   string, boolean, integer
/// Displays a popup list and returns the selected item.  May only be used
/// within a <a href="#luakeybindings">luafunc: key binding</a>.
///
/// <span class="arg">title</span> is required and captions the popup list.
///
/// <span class="arg">items</span> is a table of strings to display.
///
/// <span class="arg">index</span> optionally specifies the default item (or 1
/// if omitted).
///
/// The function returns one of the following:
/// <ul>
/// <li>nil if the popup is canceled or an error occurs.
/// <li>string indicating the <code>value</code> field from the selected item
/// (or the <code>display</code> field if no value field is present).
/// <li>boolean which is true if the item was selected with <kbd>Shift</kbd> or
/// <kbd>Ctrl</kbd> pressed.
/// <li>integer indicating the index of the selected item in the original
/// <span class="arg">items</span> table.
/// </ul>
///
/// Alternatively, the <span class="arg">items</span> argument can be a table of
/// tables with the following scheme:
/// -show:  {
/// -show:  &nbsp;   {
/// -show:  &nbsp;       value       = "...",   -- Required; this is returned if the item is chosen.
/// -show:  &nbsp;       display     = "...",   -- Optional; displayed instead of value.
/// -show:  &nbsp;       description = "...",   -- Optional; displayed in a dimmed color in a second column.
/// -show:  &nbsp;   },
/// -show:  &nbsp;   ...
/// -show:  }
///
/// The <code>value</code> field is returned if the item is chosen.
///
/// The optional <code>display</code> field is displayed in the popup list
/// instead of the <code>value</code> field.
///
/// The optional <code>description</code> field is displayed in a dimmed color
/// in a second column.  If it contains tab characters (<code>"\t"</code>) the
/// description string is split into multiple columns (up to 3).
static int popup_list(lua_State* state)
{
    if (!lua_state::is_in_luafunc())
        return luaL_error(state, "clink.popuplist may only be used in a " LUA_QL("luafunc:") " key binding");

    enum arg_indices { makevaluesonebased, argTitle, argItems, argIndex};

    const char* title = checkstring(state, argTitle);
    int index = optinteger(state, argIndex, 1) - 1;
    if (!title || !lua_istable(state, argItems))
        return 0;

    int num_items = int(lua_rawlen(state, argItems));
    if (!num_items)
        return 0;

#ifdef DEBUG
    int top = lua_gettop(state);
#endif

    std::vector<autoptr<const char>> items;
    items.reserve(num_items);
    for (int i = 1; i <= num_items; ++i)
    {
        lua_rawgeti(state, argItems, i);

        const char* value = nullptr;
        const char* display = nullptr;
        const char* description = nullptr;

        if (lua_istable(state, -1))
        {
            lua_pushliteral(state, "value");
            lua_rawget(state, -2);
            if (lua_isstring(state, -1))
                value = lua_tostring(state, -1);
            lua_pop(state, 1);

            lua_pushliteral(state, "display");
            lua_rawget(state, -2);
            if (lua_isstring(state, -1))
                display = lua_tostring(state, -1);
            lua_pop(state, 1);

            lua_pushliteral(state, "description");
            lua_rawget(state, -2);
            if (lua_isstring(state, -1))
                description = lua_tostring(state, -1);
            lua_pop(state, 1);
        }
        else
        {
            display = lua_tostring(state, -1);
        }

        if (!value && !display)
            value = display = "";
        else if (!display)
            display = value;
        else if (!value)
            value = display;

        size_t alloc_size = 3; // NUL terminators.
        alloc_size += strlen(value);
        alloc_size += strlen(display);
        if (description) alloc_size += strlen(description);

        str_moveable s;
        s.reserve(alloc_size);

        {
            char* p = s.data();
            append_string_into_buffer(p, value);
            append_string_into_buffer(p, display);
            append_string_into_buffer(p, description, true/*allow_tabs*/);
        }

        items.emplace_back(s.detach());

        lua_pop(state, 1);
    }

#ifdef DEBUG
    assert(lua_gettop(state) == top);
    assert(num_items == items.size());
#endif

    const char* choice;
    if (index > items.size()) index = items.size();
    if (index < 0) index = 0;

    popup_result result;
    if (!g_gui_popups.get())
    {
        popup_results activate_text_list(const char* title, const char** entries, int count, int current, bool has_columns);
        popup_results results = activate_text_list(title, &*items.begin(), int(items.size()), index, true/*has_columns*/);
        result = results.m_result;
        index = results.m_index;
        choice = results.m_text.c_str();
    }
    else
    {
        result = do_popup_list(title, &*items.begin(), items.size(), 0, false, false, false, index, choice, popup_items_mode::display_filter);
    }

    switch (result)
    {
    case popup_result::select:
    case popup_result::use:
        lua_pushstring(state, choice);
        lua_pushboolean(state, (result == popup_result::use));
        lua_pushinteger(state, index + 1);
        return 3;
    }

    return 0;
}

//------------------------------------------------------------------------------
/// -name:  clink.getsession
/// -ver:   1.1.44
/// -ret:   string
/// Returns the current Clink session id.
///
/// This is needed when using
/// <code><span class="hljs-built_in">io</span>.<span class="hljs-built_in">popen</span>()</code>
/// (or similar functions) to invoke <code>clink history</code> or <code>clink
/// info</code> while Clink is installed for autorun.  The popen API spawns a
/// new CMD.exe, which gets a new Clink instance injected, so the history or
/// info command will use the new session unless explicitly directed to use the
/// calling session.
/// -show:  local c = os.getalias("clink")
/// -show:  local r = io.popen(c.." --session "..clink.getsession().." history")
static int get_session(lua_State* state)
{
    str<32> session;
    session.format("%d", GetCurrentProcessId());
    lua_pushlstring(state, session.c_str(), session.length());
    return 1;
}

//------------------------------------------------------------------------------
/// -name:  clink.getansihost
/// -ver:   1.1.48
/// -ret:   string
/// Returns a string indicating who Clink thinks will currently handle ANSI
/// escape codes.  This can change based on the <code>terminal.emulation</code>
/// setting.  This always returns <code>"unknown"</code> until the first edit
/// prompt (see <a href="#clink.onbeginedit">clink.onbeginedit()</a>).
///
/// This can be useful in choosing what kind of ANSI escape codes to use, but it
/// is a best guess and is not necessarily 100% reliable.
///
/// <table>
/// <tr><th>Return</th><th>Description</th></tr>
/// <tr><td>"unknown"</td><td>Clink doesn't know.</td></tr>
/// <tr><td>"clink"</td><td>Clink is emulating ANSI support.  256 color and 24 bit color escape
///     codes are mapped to the nearest of the 16 basic colors.</td></tr>
/// <tr><td>"conemu"</td><td>Clink thinks ANSI escape codes will be handled by ConEmu.</td></tr>
/// <tr><td>"ansicon"</td><td>Clink thinks ANSI escape codes will be handled by ANSICON.</td></tr>
/// <tr><td>"winterminal"</td><td>Clink thinks ANSI escape codes will be handled by Windows
///     Terminal.</td></tr>
/// <tr><td>"winconsole"</td><td>Clink thinks ANSI escape codes will be handled by the default
///     console support in Windows, but Clink detected a terminal replacement that won't support 256
///     color or 24 bit color.</td></tr>
/// <tr><td>"winconsolev2"</td><td>Clink thinks ANSI escape codes will be handled by the default
///     console support in Windows, or it might be handled by a terminal replacement that Clink
///     wasn't able to detect.</td></tr>
/// </table>
static int get_ansi_host(lua_State* state)
{
    static const char* const s_handlers[] =
    {
        "unknown",
        "clink",
        "conemu",
        "ansicon",
        "winterminal",
        "winconsolev2",
        "winconsole",
    };

    static_assert(sizeof_array(s_handlers) == size_t(ansi_handler::max), "must match ansi_handler enum");

    size_t handler = size_t(get_current_ansi_handler());
    lua_pushstring(state, s_handlers[handler]);
    return 1;
}

//------------------------------------------------------------------------------
/// -name:  clink.translateslashes
/// -ver:   1.2.7
/// -arg:   [mode:integer]
/// -ret:   integer
/// This overrides how Clink translates slashes in completion matches, which is
/// normally determined by the <code>match.translate_slashes</code> setting.
///
/// This is reset every time match generation is invoked, so use a generator to
/// set this.
///
/// The <span class="arg">mode</span> specifies how to translate slashes when
/// generators add matches:
/// <table>
/// <tr><th>Mode</th><th>Description</th></tr>
/// <tr><td><code>0</code></td><td>No translation.</td></tr>
/// <tr><td><code>1</code></td><td>Translate using the system path separator (backslash on Windows).</td></tr>
/// <tr><td><code>2</code></td><td>Translate to slashes (<code>/</code>).</td></tr>
/// <tr><td><code>3</code></td><td>Translate to backslashes (<code>\</code>).</td></tr>
/// </table>
///
/// If <span class="arg">mode</span> is omitted, then the function returns the
/// current slash translation mode without changing it.
///
/// Note:  Clink always generates file matches using the system path separator
/// (backslash on Windows), regardless what path separator may have been typed
/// as input.  Setting this to <code>0</code> does not disable normalizing typed
/// input paths when invoking completion; it only disables translating slashes
/// in custom generators.
/// -show:  -- This example affects all match generators, by using priority -1 to
/// -show:  -- run first and returning false to let generators continue.
/// -show:  -- To instead affect only one generator, call clink.translateslashes()
/// -show:  -- in its :generate() function and return true.
/// -show:  local force_slashes = clink.generator(-1)
/// -show:  function force_slashes:generate()
/// -show:  &nbsp;   clink.translateslashes(2)  -- Convert to slashes.
/// -show:  &nbsp;   return false               -- Allow generators to continue.
/// -show:  end
static int translate_slashes(lua_State* state)
{
    extern void set_slash_translation(int mode);
    extern int get_slash_translation();

    if (lua_isnoneornil(state, 1))
    {
        lua_pushinteger(state, get_slash_translation());
        return 1;
    }

    bool isnum;
    int mode = checkinteger(state, 1, &isnum);
    if (!isnum)
        return 0;

    if (mode < 0 || mode > 3)
        mode = 1;

    set_slash_translation(mode);
    return 0;
}

//------------------------------------------------------------------------------
/// -name:  clink.slash_translation
/// -deprecated: clink.translateslashes
/// -arg:   type:integer
/// Controls how Clink will translate the path separating slashes for the
/// current path being completed. Values for <span class="arg">type</span> are;</br>
/// -1 - no translation</br>
/// 0 - to backslashes</br>
/// 1 - to forward slashes
static int slash_translation(lua_State* state)
{
    if (lua_gettop(state) == 0)
        return 0;

    if (!lua_isnumber(state, 1))
        return 0;

    int mode = int(lua_tointeger(state, 1));
    if (mode < 0)           mode = 0;
    else if (mode == 0)     mode = 3;
    else if (mode == 1)     mode = 2;
    else                    mode = 1;

    extern void set_slash_translation(int mode);
    set_slash_translation(mode);
    return 0;
}

//------------------------------------------------------------------------------
/// -name:  clink.reload
/// -ver:   1.2.29
/// Reloads Lua scripts and Readline config file at the next prompt.
static int reload(lua_State* state)
{
    force_reload_scripts();
    return 0;
}

//------------------------------------------------------------------------------
/// -name:  clink.refilterprompt
/// -ver:   1.2.46
/// Invoke the prompt filters again and refresh the prompt.
///
/// Note: this can potentially be expensive; call this only infrequently.
int g_prompt_refilter = 0;
static int refilter_prompt(lua_State* state)
{
    g_prompt_refilter++;
    void host_filter_prompt();
    host_filter_prompt();
    return 0;
}

//------------------------------------------------------------------------------
// UNDOCUMENTED; internal use only.
int g_prompt_redisplay = 0;
static int get_refilter_redisplay_count(lua_State* state)
{
    lua_pushinteger(state, g_prompt_refilter);
    lua_pushinteger(state, g_prompt_redisplay);
    return 2;
}

//------------------------------------------------------------------------------
// UNDOCUMENTED; internal use only.
static int is_transient_prompt_filter(lua_State* state)
{
    lua_pushboolean(state, prompt_filter::is_filtering());
    return 1;
}

//------------------------------------------------------------------------------
// UNDOCUMENTED; internal use only.
static int history_suggester(lua_State* state)
{
    const char* line = checkstring(state, 1);
    const int match_prev_cmd = lua_toboolean(state, 2);
    if (!line)
        return 0;

    HIST_ENTRY** history = history_list();
    if (!history || history_length <= 0)
        return 0;

    // 'match_prev_cmd' only works when 'history.dupe_mode' is 'add'.
    if (match_prev_cmd && g_dupe_mode.get() != 0)
        return 0;

    int scanned = 0;
    const DWORD tick = GetTickCount();

    const int scan_min = 200;
    const DWORD ms_max = 50;

    const char* prev_cmd = (match_prev_cmd && history_length > 0) ? history[history_length - 1]->line : nullptr;
    for (int i = history_length; --i >= 0;)
    {
        // Search at least SCAN_MIN entries.  But after that don't keep going
        // unless it's been less than MS_MAX milliseconds.
        if (scanned >= scan_min && !(scanned % 20) && GetTickCount() - tick >= ms_max)
            break;
        scanned++;

        str_iter lhs(line);
        str_iter rhs(history[i]->line);
        int matchlen = str_compare<char, false/*compute_lcd*/, true/*exact_slash*/>(lhs, rhs);

        // lhs isn't exhausted, or rhs is exhausted?  Continue searching.
        if (lhs.more() || !rhs.more())
            continue;

        // Zero matching length?  Is ok with 'match_prev_cmd', otherwise
        // continue searching.
        if (!matchlen && !match_prev_cmd)
            continue;

        // Match previous command, if needed.
        if (match_prev_cmd)
        {
            if (i <= 0 || str_compare<char, false/*compute_lcd*/, true/*exact_slash*/>(prev_cmd, history[i - 1]->line) != -1)
                continue;
        }

        // Suggest this history entry.
        lua_pushstring(state, history[i]->line);
        lua_pushinteger(state, 1);
        return 2;
    }

    return 0;
}

//------------------------------------------------------------------------------
// UNDOCUMENTED; internal use only.
static int set_suggestion_result(lua_State* state)
{
    bool isnum;
    const char* line = checkstring(state, -4);
    int endword_offset = checkinteger(state, -3, &isnum) - 1;
    if (!line || !isnum)
        return 0;

    const int line_len = strlen(line);
    if (endword_offset < 0 || endword_offset > line_len)
        return 0;

    const char* suggestion = optstring(state, -2, nullptr);
    int offset = optinteger(state, -1, 0, &isnum) - 1;
    if (!isnum || offset < 0 || offset > line_len)
        offset = line_len;

    set_suggestion(line, endword_offset, suggestion, offset);
    return 0;
}

//------------------------------------------------------------------------------
// UNDOCUMENTED; internal use only.
static int kick_idle(lua_State* state)
{
    extern void kick_idle();
    kick_idle();
    return 0;
}

//------------------------------------------------------------------------------
// UNDOCUMENTED; internal use only.
static int matches_ready(lua_State* state)
{
    bool isnum;
    int id = checkinteger(state, 1, &isnum);
    if (!isnum)
        return 0;

    extern bool notify_matches_ready(int generation_id);
    lua_pushboolean(state, notify_matches_ready(id));
    return 1;
}

//------------------------------------------------------------------------------
// UNDOCUMENTED; internal use only.
static int recognize_command(lua_State* state)
{
    const char* line = checkstring(state, 1);
    const char* word = checkstring(state, 2);
    if (!line || !word)
        return 0;
    if (!*line || !*word)
        return 0;

    // Ignore UNC paths, because they can take up to 2 minutes to time out.
    // Even running that on a thread would either starve the consumers or
    // accumulate threads faster than they can finish.
    if (path::is_separator(word[0]) && path::is_separator(word[1]))
    {
unknown:
        lua_pushinteger(state, 0);
        return 1;
    }

    // Check for directory intercepts (-, ..., ...., dir\, and so on).
    if (intercept_directory(line) != intercept_result::none)
    {
        lua_pushinteger(state, 1);
        return 1;
    }

    // Check for cached result.
    char cached;
    if (s_recognizer.find(word, &cached))
    {
known:
        lua_pushinteger(state, cached);
        return 1;
    }

    // Expand environment variables.
    str<32> expanded;
    const char* orig_word = word;
    unsigned int len = static_cast<unsigned int>(strlen(word));
    if (os::expand_env(word, len, expanded))
    {
        word = expanded.c_str();
        len = expanded.length();
    }

    // Wildcards mean it can't be an executable file.
    if (strchr(word, '*') || strchr(word, '?'))
    {
        lua_pushinteger(state, -1);
        return 1;
    }

    // Queue for background thread processing.
    if (s_recognizer.enqueue(orig_word, word, &cached))
        goto known;
    goto unknown;
}



//------------------------------------------------------------------------------
extern int set_current_dir(lua_State* state);
extern int get_aliases(lua_State* state);
extern int get_current_dir(lua_State* state);
extern int get_env(lua_State* state);
extern int get_env_names(lua_State* state);
extern int get_screen_info(lua_State* state);
extern int is_dir(lua_State* state);
extern int explode(lua_State* state);

//------------------------------------------------------------------------------
void clink_lua_initialise(lua_state& lua)
{
    struct {
        const char* name;
        int         (*method)(lua_State*);
    } methods[] = {
        // APIs in the "clink." namespace.
        { "lower",                  &to_lowercase },
        { "print",                  &clink_print },
        { "upper",                  &to_uppercase },
        { "popuplist",              &popup_list },
        { "getsession",             &get_session },
        { "getansihost",            &get_ansi_host },
        { "translateslashes",       &translate_slashes },
        { "reload",                 &reload },
        // Backward compatibility with the Clink 0.4.8 API.  Clink 1.0.0a1 had
        // moved these APIs away from "clink.", but backward compatibility
        // requires them here as well.
        { "chdir",                  &set_current_dir },
        { "execute",                &lua_execute },
        { "find_dirs",              &old_glob_dirs },
        { "find_files",             &old_glob_files },
        { "get_console_aliases",    &get_aliases },
        { "get_cwd",                &get_current_dir },
        { "get_env",                &get_env },
        { "get_env_var_names",      &get_env_names },
        { "get_host_process",       &get_host_process },
        { "get_rl_variable",        &get_rl_variable },
        { "get_screen_info",        &get_screen_info },
        { "get_setting_int",        &get_setting_int },
        { "get_setting_str",        &get_setting_str },
        { "is_dir",                 &is_dir },
        { "is_rl_variable_true",    &is_rl_variable_true },
        { "slash_translation",      &slash_translation },
        { "split",                  &explode },
        { "refilterprompt",         &refilter_prompt },
        // UNDOCUMENTED; internal use only.
        { "istransientpromptfilter", &is_transient_prompt_filter },
        { "get_refilter_redisplay_count", &get_refilter_redisplay_count },
        { "history_suggester",      &history_suggester },
        { "set_suggestion_result",  &set_suggestion_result },
        { "kick_idle",              &kick_idle },
        { "matches_ready",          &matches_ready },
        { "_recognize_command",     &recognize_command },
    };

    lua_State* state = lua.get_state();

    lua_createtable(state, sizeof_array(methods), 0);

    for (const auto& method : methods)
    {
        lua_pushstring(state, method.name);
        lua_pushcfunction(state, method.method);
        lua_rawset(state, -3);
    }

    lua_pushinteger(state, CLINK_VERSION_MAJOR * 10000000 +
                           CLINK_VERSION_MINOR *    10000 +
                           CLINK_VERSION_PATCH);
    lua_setfield(state, -2, "version_encoded");
    lua_pushinteger(state, CLINK_VERSION_MAJOR);
    lua_setfield(state, -2, "version_major");
    lua_pushinteger(state, CLINK_VERSION_MINOR);
    lua_setfield(state, -2, "version_minor");
    lua_pushinteger(state, CLINK_VERSION_PATCH);
    lua_setfield(state, -2, "version_patch");
    lua_pushstring(state, AS_STR(CLINK_COMMIT));
    lua_setfield(state, -2, "version_commit");

#ifdef DEBUG
    lua_pushboolean(state, true);
    lua_setfield(state, -2, "DEBUG");
#endif

    lua_setglobal(state, "clink");
}
