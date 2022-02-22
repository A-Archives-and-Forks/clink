// Copyright (c) 2021 Christopher Antos
// License: http://opensource.org/licenses/MIT

#pragma once

#include "line_state.h"

#include <core/str_iter.h>
#include <core/str_tokeniser.h>

#include <vector>

class line_buffer;
class collector_tokeniser;
class alias_cache;

//------------------------------------------------------------------------------
enum class collect_words_mode { stop_at_cursor, display_filter, whole_command };

//------------------------------------------------------------------------------
class word_token
{
public:
    enum : unsigned char { invalid_delim = 0xff };
                        word_token(char c, bool arg=false) : delim(c), redir_arg(arg) {}
    explicit            operator bool () const { return (delim != invalid_delim); }
    unsigned char       delim;          // Preceding delimiter.
    bool                redir_arg;      // Word is the argument of a redirection symbol.
};

//------------------------------------------------------------------------------
class collector_tokeniser
{
public:
    virtual void start(const str_iter& iter, const char* quote_pair) = 0;
    virtual word_token next(unsigned int& offset, unsigned int& length) = 0;
    virtual bool has_deprecated_argmatcher(const char* command) { return false; }
};

//------------------------------------------------------------------------------
class word_collector
{
    struct command
    {
        unsigned int        offset;
        unsigned int        length;
    };

public:
    word_collector(collector_tokeniser* command_tokeniser=nullptr, collector_tokeniser* word_tokeniser=nullptr, const char* quote_pair=nullptr);
    ~word_collector();

    void init_alias_cache();

    unsigned int collect_words(const char* buffer, unsigned int length, unsigned int cursor,
                               std::vector<word>& words, collect_words_mode mode) const;
    unsigned int collect_words(const line_buffer& buffer,
                               std::vector<word>& words, collect_words_mode mode) const;

    void collect_commands(const char* line_buffer, unsigned int line_length, unsigned int line_cursor,
                          const std::vector<word>& words, std::vector<line_state>& commands);
    void collect_commands(const line_buffer& buffer,
                          const std::vector<word>& words, std::vector<line_state>& commands);

private:
    char get_opening_quote() const;
    char get_closing_quote() const;
    void find_command_bounds(const char* buffer, unsigned int length, unsigned int cursor,
                             std::vector<command>& commands, bool stop_at_cursor) const;
    bool get_alias(const char* name, str_base& out) const;

private:
    collector_tokeniser* const m_command_tokeniser;
    collector_tokeniser* m_word_tokeniser;
    alias_cache* m_alias_cache = nullptr;
    const char* const m_quote_pair;
    bool m_delete_word_tokeniser = false;
};

//------------------------------------------------------------------------------
class simple_word_tokeniser : public collector_tokeniser
{
public:
    simple_word_tokeniser(const char* delims = " \t");
    ~simple_word_tokeniser();

    void start(const str_iter& iter, const char* quote_pair) override;
    word_token next(unsigned int& offset, unsigned int& length) override;

private:
    const char* m_delims;
    const char* m_start = nullptr;
    str_tokeniser* m_tokeniser = nullptr;
};

//------------------------------------------------------------------------------
class commands
{
public:
    commands(const char* line_buffer, unsigned int line_length, unsigned int line_cursor, const std::vector<word>& words);
    commands(const line_buffer& buffer, const std::vector<word>& words);
    const std::vector<line_state>& get_linestates() const;
private:
    std::vector<std::vector<word>> m_words_storage;
    std::vector<line_state> m_linestates;
};
