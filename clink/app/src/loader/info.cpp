// Copyright (c) 2016 Martin Ridgers
// License: http://opensource.org/licenses/MIT

#include "pch.h"
#include "utils/app_context.h"
#include "version.h"

#include <core/str.h>
#include <core/settings.h>
#include <core/os.h>
#include <core/path.h>

//------------------------------------------------------------------------------
static void print_info_line(HANDLE h, const char* s)
{
    DWORD dummy;
    if (GetConsoleMode(h, &dummy))
    {
        wstr<> tmp(s);
        DWORD written;
        WriteConsoleW(h, tmp.c_str(), tmp.length(), &written, nullptr);
    }
    else
    {
        printf("%s", s);
    }
}

//------------------------------------------------------------------------------
int clink_info(int argc, char** argv)
{
    static const struct {
        const char* name;
        void        (app_context::*method)(str_base&) const;
        bool        suppress_when_empty;
    } infos[] = {
        { "binaries",   &app_context::get_binaries_dir },
        { "state",      &app_context::get_state_dir },
        { "log",        &app_context::get_log_path },
        { "settings",   &app_context::get_settings_path },
        { "history",    &app_context::get_history_path },
        { "scripts",    &app_context::get_script_path_readable, true/*suppress_when_empty*/ },
    };

    const auto* context = app_context::get();
    const int spacing = 8;

    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);

    // Version information
    printf("%-*s : %s\n", spacing, "version", CLINK_VERSION_STR);
    printf("%-*s : %d\n", spacing, "session", context->get_id());

    // Load the settings from disk, since script paths are affected by settings.
    str<280> settings_file;
    app_context::get()->get_settings_path(settings_file);
    settings::load(settings_file.c_str());

    // Paths
    str<> s;
    for (const auto& info : infos)
    {
        str<280> out;
        (context->*info.method)(out);
        if (!info.suppress_when_empty || !out.empty())
        {
            s.clear();
            s.format("%-*s : %s\n", spacing, info.name, out.c_str());
            print_info_line(h, s.c_str());
        }
    }

    // Inputrc environment variables.
    static const char* const env_vars[] = {
        "clink_inputrc",
        "", // Magic value handled specially below.
        "userprofile",
        "localappdata",
        "appdata",
        "home"
    };

    // Inputrc file names.
    static const char *const file_names[] = {
        ".inputrc",
        "_inputrc",
        "clink_inputrc",
    };

    bool labeled = false;
    bool first = true;
    for (const char* env_var : env_vars)
    {
        bool use_state_dir = !*env_var;
        const char* label = labeled ? "" : "inputrc";
        labeled = true;
        if (use_state_dir)
            printf("%-*s : %s\n", spacing, label, "state directory");
        else
            printf("%-*s : %%%s%%\n", spacing, label, env_var);

        str<280> out;
        if (use_state_dir)
        {
            app_context::get()->get_state_dir(out);
        }
        else if (!os::get_env(env_var, out))
        {
            printf("%-*s     (unset)\n", spacing, "");
            continue;
        }

        int base_len = out.length();

        for (int i = 0; i < sizeof_array(file_names); ++i)
        {
            out.truncate(base_len);
            path::append(out, file_names[i]);

            bool exists = os::get_path_type(out.c_str()) == os::path_type_file;

            const char* status;
            if (!exists)
                status = "";
            else if (first)
                status = "   (LOAD)";
            else
                status = "   (exists)";

            if (exists || i < 2)
            {
                s.clear();
                s.format("%-*s     %s%s\n", spacing, "", out.c_str(), status);
                print_info_line(h, s.c_str());
            }

            if (exists)
                first = false;
        }
    }

    return 0;
}
