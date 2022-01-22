// Copyright (c) 2021 Christopher Antos
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "suggest.h"

#include <core/base.h>
#include <core/str.h>
#include <core/str_iter.h>
#include <core/str_compare.h>
#include <core/settings.h>
#include <core/os.h>
#include <lib/line_state.h>
#include <lib/matches.h>
#include "lua_script_loader.h"
#include "lua_state.h"
#include "line_state_lua.h"
#include "matches_lua.h"
#include "match_builder_lua.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

//------------------------------------------------------------------------------
extern matches* make_new_matches();
extern void set_suggestion(const char* line, unsigned int endword_offset, const char* suggestion, unsigned int offset);
extern setting_enum g_ignore_case;
extern setting_bool g_fuzzy_accent;
static std::shared_ptr<match_builder_toolkit> s_toolkit;

//------------------------------------------------------------------------------
void reset_suggester()
{
    s_toolkit.reset();
}

//------------------------------------------------------------------------------
match_builder_toolkit* get_deferred_matches(int generation_id)
{
    match_builder_toolkit* toolkit = s_toolkit.get();
    if (toolkit && toolkit->get_generation_id() == generation_id)
        return toolkit;
    return nullptr;
}



//------------------------------------------------------------------------------
suggester::suggester(lua_state& lua)
: m_lua(lua)
{
}

//------------------------------------------------------------------------------
bool suggester::suggest(line_state& line, matches* matches, int generation_id)
{
    s_toolkit.reset();

    if (!line.get_length())
    {
        set_suggestion("", 0, nullptr, 0);
        return true;
    }

    lua_State* state = m_lua.get_state();

    int top = lua_gettop(state);

    // Do not allow relaxed comparison for suggestions, as it is too confusing,
    // as a result of the logic to respect original case.
    int scope = g_ignore_case.get() ? str_compare_scope::caseless : str_compare_scope::exact;
    str_compare_scope compare(scope, g_fuzzy_accent.get());

    // Call Lua to filter prompt
    lua_getglobal(state, "clink");
    lua_pushliteral(state, "_suggest");
    lua_rawget(state, -2);

    line_state_lua line_lua(line);
    matches_lua matches_lua(*matches); // Doesn't deref matches, so nullptr is ok.

    // If matches not supplied, then use a coroutine to generates matches on
    // demand (if matches are not accessed, they will not be generated).
    if (matches)
    {
        line_lua.push(state);
        matches_lua.push(state);
        lua_pushnil(state);
    }
    else
    {
        s_toolkit = make_match_builder_toolkit(generation_id, line.get_end_word_offset());

        // These can't be bound to stack objects because they must stay valid
        // for the duration of the coroutine.
        line_state_lua::make_new(state, make_line_state_copy(line));
        matches_lua::make_new(state, s_toolkit);
        match_builder_lua::make_new(state, s_toolkit);
    }

    lua_pushinteger(state, generation_id);

    if (m_lua.pcall(state, 4, 1) != 0)
    {
        if (const char* error = lua_tostring(state, -1))
            m_lua.print_error(error);
        lua_settop(state, top);
        return true;
    }

    const bool cancelled = lua_isboolean(state, -1) && lua_toboolean(state, -1);

    lua_settop(state, top);
    return !cancelled;
}
